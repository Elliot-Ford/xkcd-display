#include <math.h>

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_tls.h"
#include "cJSON.h"
#include "pngle.h"
#include "esp_spi_flash.h"
#include "DEV_Config.h"
#include "EPD_7in5_V2.h"
#include "fonts.h"

#include "main.h"

#define XKCD_JSON_URL "https://xkcd.com/info.0.json"
#define XKCD_JSON "/spiffs/xkcd.json"
#define XKCD_PNG "/spiffs/xkcd.png"

#define MAX_BUFFER_LEN 1024
#define MAX_TEXT_WIDTH 80

static const char *TAG = "request";

static int get_xkcd_metadata(char **url, char **title, char **alt, int *num);
static int get_xkcd_image(char *url);
static void display_image(char *title, char *alt, int num);

static esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    switch(evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
            break;
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s",
                     evt->header_key, evt->header_value);
            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
            int mbedtls_err = 0;
            esp_err_t err = esp_tls_get_and_clear_last_error(evt->data,
                                                            &mbedtls_err, NULL);
            if (err != 0) {
                ESP_LOGI(TAG, "Last esp error code: 0x%x", err);
                ESP_LOGI(TAG, "Last mbedtls failure: 0x%x", mbedtls_err);
            }
            break;
    }
    return ESP_OK;
}

void https_get_task(void *pvParameters)
{
  int ret;
  char *image_url = NULL;
  char *title = NULL;
  char *alt = NULL;
  int old_num = -1;
  int num;

  while(1) {
    ESP_LOGI(TAG, "Starting request!");
    DEV_Module_Init();
    EPD_7IN5_V2_Init();
    ESP_LOGI(TAG, "Initialize display");
    static int request_count = 0;
    ESP_LOGI(TAG, "\tFetching metadata");
    ret = get_xkcd_metadata(&image_url, &title, &alt, &num);

    if(!ret)
    {
      if (access(XKCD_JSON, F_OK) != -1)
      {
        ESP_LOGI(TAG, "Found XKCD_JSON, reading number");
        FILE *json = fopen(XKCD_JSON, "r");
        if (json != NULL)
        {
          fread(&old_num, sizeof(int), 1, json);
          fclose(json);
        }
        ESP_LOGI(TAG, "old_num: %d", old_num);
      }
      FILE *json = fopen(XKCD_JSON, "w");
      if (json != NULL)
      {
        ESP_LOGI(TAG, "Writing number to XKCD_JSON");
        fwrite(&num, sizeof(int), 1, json);
        fclose(json);
      }
      else
      {
        ESP_LOGE(TAG, "Failed to open file for writing");
      }

      if(old_num != num)
      {
        ESP_LOGI(TAG, "\tFetching image");
        get_xkcd_image(image_url);
        free(image_url);
      }
      else
      {
        ESP_LOGI(TAG, "no new comic, no need to fetch a new image");
      }
    }

    display_image(title, alt, num);
    ESP_LOGI(TAG, "Completed %d requests", ++request_count);

    ESP_LOGI(TAG, "Delaying task execution for next 12 hours");
    vTaskDelay((12*60*60*1000) / portTICK_PERIOD_MS);
    ESP_LOGI(TAG, "Starting again!");
  }
}

static int fetch_to_file(char *url, char *fname)
{
  int content_length;
  int status_code;
  char buf[MAX_BUFFER_LEN];
  int read_len = 0;
  int ret = 0;

  esp_http_client_config_t config = {
    .event_handler = _http_event_handler,
  };
  config.url = url;

  ESP_LOGI(TAG, "Opening_file");
  FILE *f = fopen(fname, "w");
  if (f == NULL) {
    ESP_LOGE(TAG, "Failed to open file for writing");
    ret = 1;
    goto exit;
  }
  else
  {
    ESP_LOGI(TAG, "Successfully opened file. Continuing...");
  }

  esp_http_client_handle_t client = esp_http_client_init(&config);
  esp_err_t err = esp_http_client_open(client, 0);

  if(err)
  {
    ESP_LOGE(TAG, "Request failed.");
    ret = 1;
    goto cleanup;
  }

  content_length = esp_http_client_fetch_headers(client);
  status_code = esp_http_client_get_status_code(client);
  ESP_LOGI(TAG, "Status = %d, content_length = %d", status_code,
           content_length);

  while((read_len = esp_http_client_read(client, buf, MAX_BUFFER_LEN)))
  {
      fwrite(buf, sizeof(char),read_len, f);
  }

cleanup:
  fclose(f);
  esp_http_client_close(client);
  esp_http_client_cleanup(client);
exit:
  return ret;
}

static int get_xkcd_metadata(char **url, char **title, char **alt, int *num)
{
  int str_len;
  fetch_to_file(XKCD_JSON_URL, XKCD_JSON);
  ESP_LOGI(TAG, "Opening file to read JSON from");
  FILE *f = fopen(XKCD_JSON, "r");
  if(f == NULL)
  {
    ESP_LOGE(TAG, "Failed to open JSON file to read.");
    return 1;
  }

  fseek(f, 0L, SEEK_END);
  size_t buf_size = ftell(f);
  fseek(f, 0L, SEEK_SET);

  char *buf = malloc(buf_size*sizeof(char));
  fread(buf, sizeof(char), buf_size, f);
  fclose(f);

  ESP_LOGI(TAG, "Parsing as a JSON");
  // Attempt to parse the buffer as json
  cJSON *root = cJSON_Parse(buf);

  str_len = strlen(cJSON_GetObjectItem(root,"img")->valuestring)*sizeof(char);
  *url = malloc((str_len+1)*sizeof(char));
  (*url)[str_len] = '\0';
  strcpy(*url, cJSON_GetObjectItem(root,"img")->valuestring);
  ESP_LOGI(TAG, "url: %s", *url);

  str_len = strlen(cJSON_GetObjectItem(root, "safe_title")->valuestring)*sizeof(char);
  *title = malloc((str_len+1)*sizeof(char));
  (*title)[str_len] = '\0';
  strcpy(*title, cJSON_GetObjectItem(root, "safe_title")->valuestring);
  ESP_LOGI(TAG, "title: %s", *title);

  str_len = strlen(cJSON_GetObjectItem(root, "alt")->valuestring)*sizeof(char);
  *alt = malloc((str_len+1)*sizeof(char));
  (*alt)[str_len] = '\0';
  strcpy(*alt, cJSON_GetObjectItem(root, "alt")->valuestring);
  ESP_LOGI(TAG, "alt: %s", *alt);

  *num = cJSON_GetObjectItem(root, "num")->valueint;

  free(buf);

  return 0;
}

static int get_xkcd_image(char *url)
{
  return fetch_to_file(url, XKCD_PNG);
}

struct canvas_metadata
{
    int canvas_width;
    int canvas_height;
    int image_width;
    int image_height;
    unsigned char *image_bitmap;
    unsigned char *canvas;
    // TODO: We're dealing with values of 0-255 so should be able to use uint8_t here
    // (the floyd-steinberg dithering algorithm might have cause an integer overflow here).
    int *curr_line; // Buffer of the current line used to perform dithering.
    int *next_line; // Buffer of the next line used to perform dithering.
    // Comic related metadata TODO: this probably could be generalized a bit (header/footer)
    char *title;
    char *alt_text;
    int comic_num;
};

void draw_centered_text(unsigned char *canvas,
                        int            canvas_width,
                        int            canvas_height,
                        char          *str,
                        int            str_len,
                        int            y)
{
    if(y < 0 || y > canvas_height)
    {
        ESP_LOGI(TAG, "Can't draw text, y coords out of bounds");
        return;
    }
    sFONT font = Font12;
    int font_byte_width = (font.Width % 8) ? (font.Width/8 + 1) : (font.Width/8);
    int text_width = MAX_TEXT_WIDTH;

    ESP_LOGI(TAG, "str_len: %d text_width: %d", str_len, text_width);
    // If the string is larger than we're able to display let's recursivly break it down.
    if(text_width < str_len)
    {
      ESP_LOGI(TAG, "Got Here");
      while(str[text_width] != ' ') text_width--;

      // Draw first Chunk
      draw_centered_text(canvas,
                         canvas_width,
                         canvas_height,
                         str,
                         text_width,
                         y);
      // Recursivly draw the next chunk
      draw_centered_text(canvas,
                         canvas_width,
                         canvas_height,
                         &str[text_width+1],
                         str_len-text_width-1,
                         y+font.Height);
    }
    else
    {
      int x = (canvas_width - (str_len*font_byte_width)) / 2;
      for(int i=0; i<str_len; i++)
      {
        for(int j=0; j<font.Height; j++)
        {
          for(int k=0; k<font_byte_width; k++)
          {
            int font_idx = (str[i]-32)*(font.Height*font_byte_width)
                            +(j*font_byte_width)+k;
            int canvas_idx = (x+(i*font_byte_width)) + (y+j)*(canvas_width-1)+k;
            canvas[canvas_idx] = ~font.table[font_idx];
          }
        }
      }
    }
}

void init_screen(pngle_t *pngle, uint32_t w, uint32_t h)
{
  struct canvas_metadata *metadata = pngle_get_user_data(pngle);
  int x_offset;
  int y_offset;
  char *str;
  int str_len;
  /* TODO: using the given image width and height I can scale the image to the
   * display and then set the start corner of the picture */
  ESP_LOGI(TAG, "image:   w=%d h=%d", w, h);
  ESP_LOGI(TAG, "display: w=%d h=%d", EPD_7IN5_V2_WIDTH, EPD_7IN5_V2_HEIGHT);

  // Initialize the display
  metadata->image_width   = w;
  metadata->image_height  = h;
  metadata->canvas_width  = (EPD_7IN5_V2_WIDTH/8 + 1);
  metadata->canvas_height = EPD_7IN5_V2_HEIGHT;

  x_offset = (metadata->canvas_width - (metadata->image_width>>3)) / 2;
  y_offset = (metadata->canvas_height - metadata->image_height) / 2;

  // Full image can't be displayed since it is larger than the avaliable canvas
  // Currently I'm handling this w/ bounds checking in on_draw
  if(x_offset < 0 || y_offset < 0)
  {
    ESP_LOGI(TAG, "Image doesn't fit within the bounds of the canvas");
  }
  metadata->canvas = malloc((metadata->canvas_width * metadata->canvas_height)*sizeof(char));
  memset(metadata->canvas, 0xFF, metadata->canvas_width * metadata->canvas_height);
  metadata->image_bitmap = &(metadata->canvas[x_offset + (y_offset*(metadata->canvas_width-1))]);
  //metadata->image_bitmap = malloc((metadata->image_width>>3 * metadata->image_height) * sizeof(char));
  metadata->curr_line = calloc(metadata->image_width, sizeof(int));
  metadata->next_line = calloc(metadata->image_width, sizeof(int));
  ESP_LOGI(TAG, "malloc'd:   %d",
          ((metadata->image_width/8 + 1) * metadata->image_height));

  // Draw the Title and Comic number above the comic
  str_len = strlen(metadata->title);
  // Adjust the str_len to fit the possible number of places.
  str_len += 13;
  str = malloc(str_len + 1);
  str[str_len] = 0x00;
  sprintf(str, "#%d: %s", metadata->comic_num, metadata->title);

  draw_centered_text(metadata->canvas,
                     metadata->canvas_width,
                     metadata->canvas_height,
                     str,
                     strlen(str), y_offset / 2);
  free(str);

  // NOTE: this roughly centers the text as the draw_centered_text function avoids breaking up words
  // TODO: Maybe break this word-wrapping out of the draw_centered_text function.
  int lines = strlen(metadata->alt_text)/MAX_TEXT_WIDTH;
  lines += (strlen(metadata->alt_text)%MAX_TEXT_WIDTH) ? 1 : 0;

  // Draw the Alt-text under the comic
  draw_centered_text(metadata->canvas,
                     metadata->canvas_width,
                     metadata->canvas_height,
                     metadata->alt_text,
                     strlen(metadata->alt_text),
                     metadata->canvas_height - (y_offset/2) - (lines/2));

}

/**
 * Dither's the values in the (2 x length) matrix around the given position (0,i) with the rgba input
 * for the position (0,i). Function does fixed dithering from 4byte space to 1bit space.
 **/
void dither_patch(int *curr_line, int *next_line, int i, uint32_t length, uint8_t rgba[4])
{
  // Note: This method assumes no transparencies in the photo.
  if(rgba[3] < 255)
  {
    ESP_LOGI(TAG, "Pixel has transparency that's being ignored! %d", rgba[3]);
  }
  // Convert center pixel to greyscale and add the influencing values from previous passes
  int oldpixel = curr_line[i] + ( (0.3  * rgba[0])
                                + (0.59 * rgba[1])
                                + (0.11 * rgba[2]) );

  // Clip the oldpixel value to valid range
  if(oldpixel > 255) oldpixel = 255;
  if(oldpixel < 0) oldpixel = 0;


  // Covert from 1 byte greyscale to 1 bit b/w
  int newpixel = oldpixel > 127 ? 1 : 0;
  curr_line[i] = newpixel;

  // Carry forward the influencing values of the window
  int quant_error = oldpixel - (newpixel) * 255;
  if (i < length-1)
  {
    curr_line[i + 1] = curr_line[i + 1] + ((quant_error * 7)>>4);
    next_line[i + 1] = next_line[i + 1] + ((quant_error * 1)>>4);
  }
  if (i > 0)
  {
    next_line[i - 1] = next_line[i - 1] + ((quant_error * 3)>>4);
  }
  next_line[i] = next_line[i] + ((quant_error * 5)/16);
}

void on_draw(pngle_t *pngle, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
             uint8_t rgba[4])
{
  struct canvas_metadata *metadata = pngle_get_user_data(pngle);

  int *curr_line = metadata->curr_line;
  int *next_line = metadata->next_line;
  unsigned char *image_bitmap = metadata->image_bitmap;
  int image_width = metadata->image_width;
  int canvas_y = ((metadata->canvas_height - metadata->image_height) / 2) + y;
  dither_patch(curr_line, next_line, x, image_width, rgba);

  // translate current row into image bitmap
  if(x == (image_width-1))
  {
    if(canvas_y >= 0 && canvas_y < metadata->canvas_height)
    {
      for(int i = 0; i < image_width; i++)
      {
        int idx = (i / 8) + (y * (metadata->canvas_width - 1));
        if(curr_line[i])
        {
          image_bitmap[idx] |= 1<<(7-(i % 8));
        }
        else
        {
          image_bitmap[idx] &= ~(1<<(7-(i % 8)));
        }

      }
    }
    // Swap the line buffers for the next row in the image
    int *temp = curr_line;
    curr_line = next_line;
    next_line = temp;
    memset(next_line, 0x00, w*sizeof(int));
  }
}

void flush_screen(pngle_t *pngle)
{
  struct canvas_metadata *metadata = pngle_get_user_data(pngle);
  EPD_7IN5_V2_Display(metadata->canvas);
  free(metadata->canvas);
  free(metadata->curr_line);
  free(metadata->next_line);
}


static void display_image(char *title, char *alt, int num)
{
  pngle_t *pngle;
  char buf[1024];
  int remain = 0;
  int len;
  struct canvas_metadata metadata;

  metadata.title     = title;
  metadata.alt_text  = alt;
  metadata.comic_num = num;

  ESP_LOGI(TAG, "Refreshing Display");
  ESP_LOGI(TAG, "Free heap: %d\n", esp_get_free_heap_size());

  // Check if destination file exists
  struct stat st;
  if (stat(XKCD_PNG, &st) == 1) {
      ESP_LOGI(TAG, "File doesn't exist, sleeping task...");
      return;
  }
  ESP_LOGI(TAG, "File to display exists, proceeding...");
  FILE * f = fopen(XKCD_PNG, "r");

  pngle = pngle_new();
  pngle_set_user_data(pngle, &metadata);
  pngle_set_init_callback(pngle, init_screen);
  pngle_set_draw_callback(pngle, on_draw);
  pngle_set_done_callback(pngle, flush_screen);
  // Feed data to pngle
  while ((len = fread(buf, 1, sizeof(buf), f)) > 0) {
    int fed = pngle_feed(pngle, buf, 1024);
    if (fed < 0)
    {
      ESP_LOGE(TAG, "%s", pngle_error(pngle));
      break;
    }

    remain = remain + len - fed;
    if (remain > 0) memmove(buf, buf + fed, remain);
  }

  pngle_destroy(pngle);

  fclose(f);

  ESP_LOGI(TAG, "Display refreshed, sleeping task");
}

