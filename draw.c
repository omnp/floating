#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tiff.h>
#include <xcb/xcb.h>
#include <xcb/xinput.h>

#include "image.h"

#include <tiffio.h>
#include <xcb/xproto.h>

int
min (int x, int y)
{
  return x <= y ? x : y;
}

int
max (int x, int y)
{
  return x >= y ? x : y;
}

typedef struct FloatingDrawing FloatingDrawing;
typedef struct FloatingLayer FloatingLayer;
typedef struct Brush Brush;
typedef struct rect rect;

struct rect
{
  int x, y;
  int width, height;
};

struct FloatingLayer
{
  image_t *image;
  FloatingLayer *next;
  double alpha;
};

typedef
enum BlendMode
{
  BLEND_MODE_NORMAL = 0,
  BLEND_MODE_ABSORB = 1,
  BLEND_MODES,
}
BlendMode;

struct Brush
{
  double radius;
  double hardness;
  double density;
  double smudge;
  int is_drawing;
  int is_erasing;
  int is_picking;
  int is_smudging;
  BlendMode mode;
  color color;
  color medium_color;
  Brush *next;
};

struct FloatingDrawing
{
  uint32_t *image;
  double x, y;
  int is_drawing;
  FloatingLayer *bottom;
  FloatingLayer *current;
  Brush *stored_brushes; /* List of all brushes. */
  Brush *active_brushes; /* List of active ones. */
  color color;
  color medium_color;
  char *filename;
};

static double
blend (double t, double x, double y)
{
  return (1.0 - t) * x + t * y;
}

static FloatingLayer *
floating_layer_new (int width, int height)
{
  FloatingLayer *layer = malloc (sizeof (FloatingLayer));
  layer->alpha = 1.0;
  layer->image = image_new (width, height);
  layer->next = NULL;
  return layer;
}

static void
floating_layer_del (FloatingLayer *layer)
{
  image_del (layer->image);
  free (layer);
}

static void
add_top_layer (FloatingDrawing *drawing, int width, int height)
{
  FloatingLayer *current = drawing->current;
  if (current != NULL)
    {
      current->next
          = floating_layer_new (current->image->width, current->image->height);
      drawing->current = current->next;
    }
  else
    {
      drawing->current = floating_layer_new (width, height);
      drawing->bottom = drawing->current;
    }
}

static void
del_top_layer (FloatingDrawing *drawing)
{
  FloatingLayer *current = drawing->current;
  if (current != NULL)
    {
      FloatingLayer *down = drawing->bottom;
      while (down->next != NULL && down->next != current)
        {
          down = down->next;
        }
      down->next = NULL;
      drawing->current = down;
      floating_layer_del (current);
      if (current == drawing->bottom)
        {
          drawing->bottom = NULL;
          drawing->current = NULL;
        }
    }
}

void
update (FloatingDrawing *drawing, rect invalid_area, int image_width,
        int image_height, uint32_t *image, xcb_connection_t *connection,
        xcb_window_t window, xcb_gcontext_t draw, xcb_pixmap_t pixmap,
        uint8_t background)
{
  /* Draw to screen. */
  if (invalid_area.x < image_width && invalid_area.y < image_height
      && invalid_area.width && invalid_area.height)
    {
      const int width = invalid_area.width;
      const int height = invalid_area.height;
      const int x = invalid_area.x;
      const int y = invalid_area.y;
      image_t *scratch = image_new (width, height);
      FloatingLayer *current = drawing->bottom;

      while (current != NULL)
        {
          int i;
#pragma omp parallel for
          for (i = 0; i < height; ++i)
            {
              int j;
#pragma omp parallel for
              for (j = 0; j < width; ++j)
                {
                  unsigned int scratch_index = i * width + j;
                  unsigned int current_index
                      = (y + i) * current->image->width + x + j;
                  if (x + j >= 0 && x + j < image_width && y + i < image_height
                      && y + i >= 0)
                    {
                      color final_color = scratch->data[scratch_index];
                      color current_color
                          = current->image->data[current_index];
                      if (current != drawing->bottom)
                        {
                          if (current->alpha > 0 && current_color.alpha > 0)
                            {
                              const float alpha = final_color.alpha;
                              const float current_alpha = current_color.alpha;
                              const float inv_alpha
                                  = 1
                                    / (alpha * (1 - current_alpha)
                                       + current->alpha * current_alpha);
                              color_multiply_single_struct (final_color.alpha,
                                                            final_color.vector,
                                                            &final_color);
                              color_multiply_single_struct (
                                  current->alpha, current_color.vector,
                                  &current_color);
                              color_blend_absorb_single (
                                  current_alpha, &final_color,
                                  &current_color, &final_color);
                              color_multiply_single_struct (
                                  inv_alpha, final_color.vector, &final_color);
                              final_color.alpha = fmin (
                                  1.0, alpha + current->alpha * current_alpha);
                            }
                        }
                      else
                        {
                          final_color = current_color;
                          final_color.alpha
                              = current->alpha * final_color.alpha;
                        }
                      scratch->data[scratch_index] = final_color;
                    }
                }
            }
          current = current->next;
        }

      uint8_t *tmp_data = malloc (sizeof (uint32_t) * width * height);
      unsigned char *surface_data = (void *)image;
      int i;
#pragma omp parallel for
      for (i = 0; i < height; ++i)
        {
          int j;
#pragma omp parallel for
          for (j = 0; j < width; ++j)
            {
              int scratch_index = i * width + j;
              int surface_index = 4 * ((y + i) * image_width + x + j);
              if (x + j >= 0 && x + j < image_width && y + i < image_height
                  && y + i >= 0)
                {
                  const double f = scratch->data[scratch_index].alpha;
                  surface_data[surface_index + 0]
                      = scratch->data[scratch_index].red * 255;
                  surface_data[surface_index + 1]
                      = scratch->data[scratch_index].green * 255;
                  surface_data[surface_index + 2]
                      = scratch->data[scratch_index].blue * 255;
                  surface_data[surface_index + 3]
                      = scratch->data[scratch_index].alpha * 255;
                  tmp_data[4 * scratch_index + 0]
                      = blend (f, background, surface_data[surface_index + 2]);
                  tmp_data[4 * scratch_index + 1]
                      = blend (f, background, surface_data[surface_index + 1]);
                  tmp_data[4 * scratch_index + 2]
                      = blend (f, background, surface_data[surface_index + 0]);
                  tmp_data[4 * scratch_index + 3]
                      = blend (f, background, surface_data[surface_index + 3]);
                }
            }
        }

      image_del (scratch);

      xcb_put_image (connection, XCB_IMAGE_FORMAT_Z_PIXMAP, pixmap, draw,
                     width, height, x, y, 0, 24, (4 * width * height),
                     (void *)tmp_data);
      xcb_copy_area (connection, pixmap, window, draw, x, y, x, y, width,
                     height);
      xcb_flush (connection);
      free (tmp_data);
    }
}

#define BRUSH_SIZE_MAX 64
#define BRUSH_SIZE_DEFAULT 20

#define BACKGROUND 0x40

const color Red = { { 1.0, 0.0, 0.0, 1.0 } };
const color Green = { { 0.0, 1.0, 0.0, 1.0 } };
const color Blue = { { 0.0, 0.0, 1.0, 1.0 } };
const color White = { { 1.0, 1.0, 1.0, 1.0 } };
const color Black = { { 0.0, 0.0, 0.0, 1.0 } };
const color Gray = { { 0.5, 0.5, 0.5, 1.0 } };
const color Cyan = { { 0.0, 1.0, 1.0, 1.0 } };
const color Magenta = { { 1.0, 0.0, 1.0, 1.0 } };
const color Yellow = { { 1.0, 1.0, 0.0, 1.0 } };

int
main (int argc, char **args)
{
  const color *Colors[] =
    {
      &Red, &Green, &Blue, &White, &Black, &Gray, &Cyan, &Magenta, &Yellow
    };
  const int colors = 9; /*length of Colors*/
  int image_width = 400;
  int image_height = 400;
  char *image_file_name = 0;
  if (argc > 3)
    {
      image_width = atoi (args[1]);
      image_height = atoi (args[2]);
      image_file_name = args[3];
    }
  FloatingDrawing drawing_obj;
  Brush default_brush;
  color default_color = { { 1, 0.1, 0.25, 0.8 } };
  color default_medium_color = { { 0.9, 0.9, 0.75, 0.0 } };
  default_brush.is_drawing = 0;
  default_brush.is_picking = 0;
  default_brush.is_erasing = 0;
  default_brush.is_smudging = 0;
  default_brush.mode = BLEND_MODE_NORMAL;
  default_brush.color = default_color;
  default_brush.medium_color = default_medium_color;
  default_brush.radius = BRUSH_SIZE_DEFAULT;
  default_brush.hardness = 0.4;
  default_brush.density = 2.5;
  default_brush.smudge = 0.5;
  default_brush.next = NULL;
  drawing_obj.image = NULL;
  drawing_obj.is_drawing = 0;
  drawing_obj.bottom = floating_layer_new (image_width, image_height);
  drawing_obj.current = drawing_obj.bottom;
  drawing_obj.stored_brushes = &default_brush;
  drawing_obj.active_brushes = &default_brush;
  drawing_obj.filename = image_file_name;
  drawing_obj.color = default_color;
  drawing_obj.medium_color = default_medium_color;
  FloatingDrawing *drawing = &drawing_obj;

  uint8_t graphics_tablet_stylus_device_id
      = 0; /* Will find what the correct id is later. */
  const int graphics_tablet_stylus_pressure_axis_number = 3;
  double graphics_tablet_stylus_pressure_axis_min = 0;
  double graphics_tablet_stylus_pressure_axis_max = 1;
  uint32_t graphics_tablet_stylus_pressure_axis_resolution = 1;
  const int graphics_tablet_stylus_x_axis_number = 1;
  double graphics_tablet_stylus_x_axis_min = 0;
  double graphics_tablet_stylus_x_axis_max = 1;
  uint32_t graphics_tablet_stylus_x_axis_resolution = 1;
  const int graphics_tablet_stylus_y_axis_number = 2;
  double graphics_tablet_stylus_y_axis_min = 0;
  double graphics_tablet_stylus_y_axis_max = 1;
  uint32_t graphics_tablet_stylus_y_axis_resolution = 1;

  xcb_connection_t *connection = xcb_connect (NULL, NULL);
  xcb_screen_t *screen
      = xcb_setup_roots_iterator (xcb_get_setup (connection)).data;
  xcb_drawable_t window;
  xcb_gcontext_t draw = xcb_generate_id (connection);
  uint32_t mask;
  uint32_t values[2];

  window = xcb_generate_id (connection);
  mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
  values[0] = 0x808080;
  values[1] = XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_PROPERTY_CHANGE
              | XCB_EVENT_MASK_STRUCTURE_NOTIFY
              | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY
              | XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_BUTTON_PRESS
              | XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_KEY_PRESS;

  xcb_create_window (connection, XCB_COPY_FROM_PARENT, window, screen->root, 0,
                     0, image_width, image_height, 0,
                     XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, mask,
                     values);

  xcb_map_window (connection, window);

  uint32_t *image = malloc (sizeof (uint32_t) * image_width * image_height);
  memset ((void *)image, BACKGROUND,
          sizeof (uint32_t) * image_width * image_height);

  int i;
  for (i = 0; i < image_width * image_height; ++i)
    {
      *((unsigned char *)(image + i) + 3) = 0x00;
    }

  drawing->image = image;

  xcb_pixmap_t pixmap = xcb_generate_id (connection);
  xcb_create_pixmap (connection, 24, pixmap, window, image_width,
                     image_height);

  mask = XCB_GC_GRAPHICS_EXPOSURES;
  values[0] = 0;
  values[1] = 0;
  xcb_create_gc (connection, draw, window, mask, values);

  printf ("Screen depth: %d\n", screen->root_depth);
  xcb_put_image (connection, XCB_IMAGE_FORMAT_Z_PIXMAP, pixmap, draw,
                 image_width, image_height, 0, 0, 0, 24,
                 (4 * image_width * image_height), (void *)image);
  xcb_flush (connection);

  const double divider = (double)(1ull << 32);
  xcb_input_xi_query_device_reply_t *devices_reply;
  xcb_input_xi_query_device_cookie_t devices_cookie;
  devices_cookie
      = xcb_input_xi_query_device (connection, XCB_INPUT_DEVICE_ALL);
  devices_reply
      = xcb_input_xi_query_device_reply (connection, devices_cookie, NULL);
  xcb_input_xi_device_info_iterator_t devices_iterator
      = xcb_input_xi_query_device_infos_iterator (devices_reply);
  for (; devices_iterator.rem;
       xcb_input_xi_device_info_next (&devices_iterator))
    {
      xcb_input_xi_device_info_t *info = devices_iterator.data;
      char *name = xcb_input_xi_device_info_name (info);
      printf ("%d %d %s %d %d\n", info->deviceid, info->type, name,
              info->attachment, info->num_classes);
      if (strstr(name, "stylus") || strstr(name, "Stylus"))
        {
          graphics_tablet_stylus_device_id = info->deviceid;
          printf ("Found %s device with device id = %d\n",
                  name, info->deviceid);
          xcb_input_device_class_iterator_t class_iterator
              = xcb_input_xi_device_info_classes_iterator (info);
          for (; class_iterator.rem;
               xcb_input_device_class_next (&class_iterator))
            {
              xcb_input_device_class_t *class = class_iterator.data;
              if (class->type == XCB_INPUT_DEVICE_CLASS_TYPE_VALUATOR)
                {
                  xcb_input_valuator_class_t *class_data = (void *)class;
                  if (class_data)
                    {
                      {
                        double min, max, range;
                        uint32_t res = class_data->resolution;
                        min = (((double)class_data->min.integral)
                               + ((double)class_data->min.frac) / divider);
                        max = (((double)class_data->max.integral)
                               + ((double)class_data->max.frac) / divider);
                        range = max - min;
                        printf ("%d res = %d min = %f max = %f range = %f\n",
                                class_data->number, res, min, max, range);
                      }
                      if (class_data->number + 1
                          == graphics_tablet_stylus_pressure_axis_number)
                        {
                          graphics_tablet_stylus_pressure_axis_resolution
                              = class_data->resolution;
                          graphics_tablet_stylus_pressure_axis_min
                              = (((double)class_data->min.integral)
                                 + ((double)class_data->min.frac) / divider);
                          graphics_tablet_stylus_pressure_axis_max
                              = (((double)class_data->max.integral)
                                 + ((double)class_data->max.frac) / divider);
                          printf (
                              "Pressure axis number %d with min = %f, max = "
                              "%f, and resolution = %d, range = %f\n",
                              graphics_tablet_stylus_pressure_axis_number,
                              graphics_tablet_stylus_pressure_axis_min,
                              graphics_tablet_stylus_pressure_axis_max,
                              graphics_tablet_stylus_pressure_axis_resolution,
                              graphics_tablet_stylus_pressure_axis_max
                                  - graphics_tablet_stylus_pressure_axis_min);
                        }
                      if (class_data->number + 1
                          == graphics_tablet_stylus_x_axis_number)
                        {
                          graphics_tablet_stylus_x_axis_resolution
                              = class_data->resolution;
                          graphics_tablet_stylus_x_axis_min
                              = (((double)class_data->min.integral)
                                 + ((double)class_data->min.frac) / divider);
                          graphics_tablet_stylus_x_axis_max
                              = (((double)class_data->max.integral)
                                 + ((double)class_data->max.frac) / divider);
                          printf ("X axis number %d with min = %f, max = %f, "
                                  "and resolution = %d, range = %f\n",
                                  graphics_tablet_stylus_x_axis_number,
                                  graphics_tablet_stylus_x_axis_min,
                                  graphics_tablet_stylus_x_axis_max,
                                  graphics_tablet_stylus_x_axis_resolution,
                                  graphics_tablet_stylus_x_axis_max
                                      - graphics_tablet_stylus_x_axis_min);
                        }
                      if (class_data->number + 1
                          == graphics_tablet_stylus_y_axis_number)
                        {
                          graphics_tablet_stylus_y_axis_resolution
                              = class_data->resolution;
                          graphics_tablet_stylus_y_axis_min
                              = (((double)class_data->min.integral)
                                 + ((double)class_data->min.frac) / divider);
                          graphics_tablet_stylus_y_axis_max
                              = (((double)class_data->max.integral)
                                 + ((double)class_data->max.frac) / divider);
                          printf ("Y axis number %d with min = %f, max = %f, "
                                  "and resolution = %d, range = %f\n",
                                  graphics_tablet_stylus_y_axis_number,
                                  graphics_tablet_stylus_y_axis_min,
                                  graphics_tablet_stylus_y_axis_max,
                                  graphics_tablet_stylus_y_axis_resolution,
                                  graphics_tablet_stylus_y_axis_max
                                      - graphics_tablet_stylus_y_axis_min);
                        }
                    }
                }
            }
        }
    }
  struct
  {
    xcb_input_event_mask_t iem;
    xcb_input_xi_event_mask_t xiem;
  } se_mask;
  se_mask.iem.deviceid = XCB_INPUT_DEVICE_ALL;
  se_mask.iem.mask_len = 1;
  se_mask.xiem = XCB_INPUT_XI_EVENT_MASK_MOTION
                 | XCB_INPUT_XI_EVENT_MASK_BUTTON_PRESS
                 | XCB_INPUT_XI_EVENT_MASK_BUTTON_RELEASE;
  xcb_input_xi_select_events (connection, window, 1, &se_mask.iem);
  xcb_flush (connection);
  if (graphics_tablet_stylus_device_id)
    {
      xcb_input_xi_passive_grab_device_cookie_t cookie;
      xcb_input_xi_passive_grab_device_reply_t *reply;
      const uint32_t mask[]
          = { XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_BUTTON_PRESS
              | XCB_EVENT_MASK_BUTTON_RELEASE };
      const uint32_t mods[] = { XCB_INPUT_MODIFIER_MASK_ANY };
      cookie = xcb_input_xi_passive_grab_device (
          connection, XCB_CURRENT_TIME, screen->root, XCB_CURSOR_NONE, 0,
          graphics_tablet_stylus_device_id, 1, 1, XCB_INPUT_GRAB_TYPE_FOCUS_IN,
          XCB_INPUT_GRAB_MODE_22_ASYNC, XCB_INPUT_GRAB_MODE_22_ASYNC,
          XCB_INPUT_GRAB_OWNER_NO_OWNER, mask, mods);
      reply
          = xcb_input_xi_passive_grab_device_reply (connection, cookie, NULL);
      free (reply);
      struct
      {
        xcb_input_event_mask_t iem;
        xcb_input_xi_event_mask_t xiem;
      } se_mask;
      se_mask.iem.deviceid = graphics_tablet_stylus_device_id;
      se_mask.iem.mask_len = 1;
      se_mask.xiem = XCB_INPUT_XI_EVENT_MASK_MOTION
                     | XCB_INPUT_XI_EVENT_MASK_RAW_MOTION
                     | XCB_INPUT_XI_EVENT_MASK_BUTTON_PRESS
                     | XCB_INPUT_XI_EVENT_MASK_BUTTON_RELEASE
                     | XCB_INPUT_XI_EVENT_MASK_RAW_BUTTON_PRESS
                     | XCB_INPUT_XI_EVENT_MASK_RAW_BUTTON_RELEASE;
      xcb_input_xi_select_events (connection, screen->root, 1, &se_mask.iem);
      xcb_flush (connection);
    }

  uint16_t root_width, root_height;
  root_width = screen->width_in_pixels;
  root_height = screen->height_in_pixels;

  xcb_generic_event_t *event;
  uint16_t win_original_conf_x = 0, win_original_conf_y = 0;
  uint16_t win_pos_x = 0, win_pos_y = 0;
  float pressure = 0.0f;
  int colors_index = -1;
  BlendMode blend_mode = BLEND_MODE_NORMAL;
  while ((event = xcb_wait_for_event (connection)))
    {
      switch (event->response_type & ~0x80)
        {
        case XCB_CONFIGURE_NOTIFY:
          {
            xcb_configure_notify_event_t *nt_event = (void *)event;
            if (!win_original_conf_x)
              {
                win_original_conf_x = nt_event->x;
              }
            if (!win_original_conf_y)
              {
                win_original_conf_y = nt_event->y;
              }
            if (win_original_conf_x != nt_event->x)
              {
                win_pos_x = nt_event->x;
              }
            if (win_original_conf_y != nt_event->y)
              {
                win_pos_y = nt_event->y;
              }
            break;
          }
        case XCB_GE_GENERIC:
          {
            const double prev_x = drawing->x;
            const double prev_y = drawing->y;
            xcb_ge_generic_event_t *ge_event = (void *)event;
            switch (ge_event->event_type)
              {
              case XCB_INPUT_RAW_BUTTON_PRESS:
                {
                  xcb_input_raw_button_press_event_t *rbt_event
                      = (void *)event;
                  if (rbt_event->deviceid > 3
                      && rbt_event->deviceid
                             == graphics_tablet_stylus_device_id)
                    {
                      if (rbt_event->detail == 1)
                        {
                          drawing->is_drawing = 1;
                          pressure = 1.0;
                          int axis_len
                              = xcb_input_raw_button_press_axisvalues_length (
                                  rbt_event);
                          if (axis_len)
                            {
                              xcb_input_fp3232_t *axisvalues
                                  = xcb_input_raw_button_press_axisvalues_raw (
                                      rbt_event);
                              int i = 0;
                              for (; i < axis_len; ++i)
                                {
                                  xcb_input_fp3232_t value = axisvalues[i];
                                  double dbl_value
                                      = ((double)value.integral
                                         + (double)value.frac / divider);
                                  if (i
                                      == graphics_tablet_stylus_x_axis_number
                                             - 1)
                                    {
                                      long pos_x
                                          = root_width * dbl_value
                                            / (graphics_tablet_stylus_x_axis_max
                                               - graphics_tablet_stylus_x_axis_min);
                                      drawing->x = pos_x - win_pos_x;
                                    }
                                  if (i
                                      == graphics_tablet_stylus_y_axis_number
                                             - 1)
                                    {
                                      long pos_y
                                          = root_height * dbl_value
                                            / (graphics_tablet_stylus_y_axis_max
                                               - graphics_tablet_stylus_y_axis_min);
                                      drawing->y = pos_y - win_pos_y;
                                    }
                                  if (i
                                      == graphics_tablet_stylus_pressure_axis_number
                                             - 1)
                                    {
                                      pressure
                                          = pressure
                                            * (float)(dbl_value
                                                      / graphics_tablet_stylus_pressure_axis_max);
                                    }
                                }
                            }
                        }
                    }
                  break;
                }
              case XCB_INPUT_RAW_BUTTON_RELEASE:
                {
                  xcb_input_raw_button_release_event_t *rbt_event
                      = (void *)event;
                  if (rbt_event->deviceid > 3
                      && rbt_event->deviceid
                             == graphics_tablet_stylus_device_id)
                    {
                      if (rbt_event->detail == 1)
                        {
                          drawing->is_drawing = 0;
                          pressure = 0.0;
                        }
                    }
                  break;
                }
              case XCB_INPUT_RAW_MOTION:
                {
                  xcb_input_raw_motion_event_t *rmt_event = (void *)event;
                  if (rmt_event->deviceid > 3
                      && rmt_event->deviceid
                             == graphics_tablet_stylus_device_id)
                    {
                      int axis_len
                          = xcb_input_raw_button_press_axisvalues_length (
                              rmt_event);
                      if (axis_len)
                        {
                          xcb_input_fp3232_t *axisvalues
                              = xcb_input_raw_button_press_axisvalues_raw (
                                  rmt_event);
                          int i = 0;
                          for (; i < axis_len; ++i)
                            {
                              xcb_input_fp3232_t value = axisvalues[i];
                              double dbl_value
                                  = ((double)value.integral
                                     + (double)value.frac / divider);
                              if (i
                                  == graphics_tablet_stylus_x_axis_number - 1)
                                {
#ifndef WAYLAND
                                  long pos_x
                                      = root_width * dbl_value
                                        / (graphics_tablet_stylus_x_axis_max
                                           - graphics_tablet_stylus_x_axis_min);
                                  drawing->x = pos_x - win_pos_x;
#else
                                  long pos_x
                                      = dbl_value;
                                  drawing->x = pos_x - win_pos_x;
#endif
                                }
                              if (i
                                  == graphics_tablet_stylus_y_axis_number - 1)
                                {
#ifndef WAYLAND
                                  long pos_y
                                      = root_height * dbl_value
                                        / (graphics_tablet_stylus_y_axis_max
                                           - graphics_tablet_stylus_y_axis_min);
                                  drawing->y = pos_y - win_pos_y;
#else
                                  long pos_y
                                      = dbl_value;
                                  drawing->y = pos_y - win_pos_y;
#endif
                                }
                              if (i
                                  == graphics_tablet_stylus_pressure_axis_number
                                         - 1)
                                {
                                  pressure
                                      = (float)(dbl_value
                                                / graphics_tablet_stylus_pressure_axis_max);
                                }
                            }
                        }
                    }
                  break;
                }
              case XCB_INPUT_BUTTON_PRESS:
                {
                  xcb_input_button_press_event_t *bt_event = (void *)event;
                  if (bt_event->deviceid != graphics_tablet_stylus_device_id)
                    {
                      uint32_t *button_mask
                          = xcb_input_button_press_button_mask (bt_event);
                      int button_len
                          = xcb_input_button_press_button_mask_length (
                              bt_event);
                      if (bt_event->deviceid > 3 && button_len
                          && bt_event->detail == 1 && !button_mask[0])
                        {
                          drawing->is_drawing = 1;
                          pressure = 1.0;
                          win_pos_x = (bt_event->root_x >> 16)
                                      - (bt_event->event_x >> 16);
                          win_pos_y = (bt_event->root_y >> 16)
                                      - (bt_event->event_y >> 16);
                          drawing->x = (bt_event->event_x >> 16);
                          drawing->y = (bt_event->event_y >> 16);
                        }
                    }
                  break;
                }
              case XCB_INPUT_BUTTON_RELEASE:
                {
                  xcb_input_button_release_event_t *bt_event = (void *)event;
                  if (bt_event->deviceid != graphics_tablet_stylus_device_id)
                    {
                      uint32_t *button_mask
                          = xcb_input_button_press_button_mask (bt_event);
                      int button_len
                          = xcb_input_button_press_button_mask_length (
                              bt_event);
                      if (bt_event->deviceid > 3 && button_len
                          && bt_event->detail == 1 && button_mask[0])
                        {
                          win_pos_x = (bt_event->root_x >> 16)
                                      - (bt_event->event_x >> 16);
                          win_pos_y = (bt_event->root_y >> 16)
                                      - (bt_event->event_y >> 16);
                          drawing->is_drawing = 0;
                          pressure = 0.0;
                        }
                    }
                  break;
                }
              case XCB_INPUT_MOTION:
                {
                  xcb_input_motion_event_t *mt_event = (void *)event;
                  if (mt_event->deviceid > 3
                      && mt_event->deviceid
                             != graphics_tablet_stylus_device_id)
                    {
                      drawing->x = (mt_event->root_x >> 16) - win_pos_x;
                      drawing->y = (mt_event->root_y >> 16) - win_pos_y;
                    }
                  break;
                }
              default:
                break;
              }
            if (!drawing->is_drawing || drawing->current == NULL
                || drawing->current->image == NULL)
              {
                break;
              }
            if (pressure <= 0.0)
              {
                break;
              }
            /* Draw to image buffer. */
            const int width = drawing->current->image->width;
            const int height = drawing->current->image->height;
            color *pix = drawing->current->image->data;
            Brush *brush = drawing->active_brushes;
            rect invalid_area;
            invalid_area.x = image_width;
            invalid_area.y = image_height;
            invalid_area.width = 0;
            invalid_area.height = 0;
            while (brush != NULL)
              {
                double brush_density = brush->density;
                double brush_radius = brush->radius * pressure;
                double brush_hardness = brush->hardness;
                double brush_alpha = 1.0;
                double brush_smudge = brush->smudge * pressure;
                if (brush->is_drawing && brush_density > 0 && brush_radius > 0
                    && brush_hardness > 0)
                  {
                    double t;
                    for (t = 0.0; t < 1.0 + brush_density; t += brush_density)
                      {
                        const double x = t * drawing->x + (1 - t) * prev_x;
                        const double y = t * drawing->y + (1 - t) * prev_y;
                        const int xi = x, yi = y;
                        const double brush_radius_sq
                            = brush_radius * brush_radius;
                        unsigned int total_pixels = 0;
                        color total_color = { { 0, 0, 0, 0 } };
                        /* Draw circular brush mark. */
                        const int brush_bounding_size
                            = 2 * ceil (brush_radius) + 1;
                        int i;
#pragma omp parallel for
                        for (i = xi - ceil (brush_radius);
                             i <= xi + brush_bounding_size; ++i)
                          {
                            int j;
#pragma omp parallel for
                            for (j = yi - ceil (brush_radius);
                                 j <= yi + brush_bounding_size; ++j)
                              {
                                double blend_factor = 0.0;
                                double alpha = 1.0;
                                double distance_sq
                                    = (i - x) * (i - x) + (j - y) * (j - y);
                                if (i >= 0 && j >= 0 && i < width && j < height
                                    && distance_sq <= brush_radius_sq)
                                  {
                                    unsigned int index = (j * width + i);
                                    color final_color = pix[index];
                                    color brush_color;
                                    switch (brush->mode)
                                      {
                                        case BLEND_MODE_ABSORB:
                                        color_blend_absorb_single (
                                            brush->medium_color.alpha,
                                            &brush->color,
                                            &brush->medium_color,
                                            &brush_color);
                                        break;
                                        case BLEND_MODE_NORMAL:
                                        default:
                                        color_blend_single_struct (
                                            brush->medium_color.alpha,
                                            brush->color.vector,
                                            brush->medium_color.vector,
                                            &brush_color);
                                        break;
                                      }
                                    if (distance_sq / brush_radius_sq
                                        >= brush_hardness)
                                      {
                                        alpha = brush_hardness * brush_hardness
                                                * brush_radius_sq
                                                / distance_sq;
                                      }
                                    alpha *= brush_alpha;
                                    if (brush->is_erasing)
                                      {
                                        if (final_color.alpha > 0)
                                          {
                                            blend_factor
                                                = alpha * brush_color.alpha;
                                            if (final_color.alpha > 0)
                                              {
                                                final_color.alpha = blend (
                                                    blend_factor,
                                                    final_color.alpha, 0);
                                              }
                                          }
                                      }
                                    else
                                      {
                                        if (final_color.alpha > 0)
                                          {
                                            blend_factor
                                                = alpha * brush_color.alpha;
                                            switch (brush->mode)
                                              {
                                                case BLEND_MODE_ABSORB:
                                                color_blend_absorb_single (
                                                    blend_factor,
                                                    &final_color,
                                                    &brush_color,
                                                    &final_color);
                                                break;
                                                case BLEND_MODE_NORMAL:
                                                default:
                                                color_blend_single_struct (
                                                    blend_factor,
                                                    final_color.vector,
                                                    brush_color.vector,
                                                    &final_color);
                                                break;
                                              }
                                          }
                                        else
                                          {
                                            final_color = brush_color;
                                            final_color.alpha
                                                = alpha * final_color.alpha;
                                          }
#pragma omp critical
                                        {
                                          color_add_struct (total_color.vector,
                                                            final_color.vector,
                                                            &total_color);
                                          total_pixels += 1;
                                        }
                                      }
                                    if (!brush->is_picking)
                                      {
                                        pix[index].red = final_color.red;
                                        pix[index].green = final_color.green;
                                        pix[index].blue = final_color.blue;
                                        pix[index].alpha = final_color.alpha;
                                      }
                                  }
                              }
                          }
                        if (total_pixels > 0)
                          {
                            total_color.red /= total_pixels;
                            total_color.green /= total_pixels;
                            total_color.blue /= total_pixels;
                            total_color.alpha /= total_pixels;
                            if (brush->is_smudging)
                              {
                                switch (brush->mode)
                                  {
                                    case BLEND_MODE_ABSORB:
                                    color_blend_absorb_single (
                                        brush_smudge, &brush->color,
                                        &total_color, &brush->color);
                                    break;
                                    case BLEND_MODE_NORMAL:
                                    default:
                                    color_blend_absorb_single (
                                        brush_smudge, &brush->color,
                                        &total_color, &brush->color);
                                    break;
                                  }
                              }
                            if (brush->is_picking)
                              {
                                drawing->color = total_color;
                                brush->color = drawing->color;
                              }
                          }
                        int invalid_area_x = xi - brush_radius;
                        int invalid_area_y = yi - brush_radius;
                        while (width - invalid_area_x < brush_bounding_size)
                          {
                            --invalid_area_x;
                          }
                        while (height - invalid_area_y < brush_bounding_size)
                          {
                            --invalid_area_y;
                          }
                        int invalid_area_width = brush_bounding_size;
                        int invalid_area_height = brush_bounding_size;
                        invalid_area.x = min (invalid_area.x, invalid_area_x);
                        invalid_area.y = min (invalid_area.y, invalid_area_y);
                        invalid_area.width
                            = max (invalid_area.width, invalid_area_width);
                        invalid_area.height
                            = max (invalid_area.height, invalid_area_height);
                      }
                  }
                brush = brush->next;
              }
            /* Draw to screen and update image file buffer. */
            update (drawing, invalid_area, image_width, image_height, image,
                    connection, window, draw, pixmap, BACKGROUND);
            break;
          }
        case XCB_EXPOSE:
          {
            xcb_copy_area (connection, pixmap, window, draw, 0, 0, 0, 0,
                           image_width, image_height);
            xcb_flush (connection);
            break;
          }
        case XCB_KEY_PRESS:
          {
            xcb_key_press_event_t *key_event = (void *)event;
            printf ("Keycode: %d, %d\n", key_event->detail, key_event->state);
            switch (key_event->detail)
              {
              case 31:
                { /*key: i; increase brush size*/
                  if (key_event->state & XCB_MOD_MASK_SHIFT)
                    { /*key: shift-i; decrease brush size*/
                      Brush *brush = drawing->active_brushes;
                      while (brush != NULL)
                        {
                          brush->radius -= 1;
                          if (brush->radius < 0)
                            {
                              brush->radius = 0;
                            }
                          brush = brush->next;
                        }
                      /*printf("Maybe decreased brush size to %d\n",
                       * brush->size);*/
                    }
                  else
                    {
                      Brush *brush = drawing->active_brushes;
                      while (brush != NULL)
                        {
                          brush->radius += 1;
                          if (brush->radius > BRUSH_SIZE_MAX)
                            {
                              brush->radius = BRUSH_SIZE_MAX;
                            }
                          brush = brush->next;
                        }
                      /*printf("Maybe increased brush size to %d\n",
                       * brush->size);*/
                    }
                  break;
                }
              case 32:
                { /*key: o; increase brush alpha*/
                  if (key_event->state & XCB_MOD_MASK_SHIFT)
                    { /*key: shift-o; decrease brush alpha*/
                      Brush *brush = drawing->active_brushes;
                      while (brush != NULL)
                        {
                          brush->color.alpha -= 0.01;
                          if (brush->color.alpha < 0)
                            {
                              brush->color.alpha = 0;
                            }
                          brush = brush->next;
                        }
                      /*printf("Maybe decreased brush alpha to %d\n",
                       * brush->color.alpha);*/
                    }
                  else
                    {
                      Brush *brush = drawing->active_brushes;
                      while (brush != NULL)
                        {
                          brush->color.alpha += 0.01;
                          if (brush->color.alpha > 1)
                            {
                              brush->color.alpha = 1;
                            }
                          brush = brush->next;
                        }
                      /*printf("Maybe increased brush alpha to %d\n",
                       * brush->color.alpha);*/
                    }
                  break;
                }
              case 39:
                { /*key: s; maybe save image file*/
                  if (key_event->state & XCB_MOD_MASK_SHIFT)
                    { /*shift-s saves image data to file*/
                      if (image_file_name)
                        {
                          TIFF *tif = TIFFOpen (image_file_name, "w");
                          if (tif)
                            {
                              int32_t x, y;
                              const uint16_t extra[]
                                  = { EXTRASAMPLE_UNASSALPHA };
                              TIFFSetField (tif, TIFFTAG_IMAGEWIDTH,
                                            image_width);
                              TIFFSetField (tif, TIFFTAG_IMAGELENGTH,
                                            image_height);
                              TIFFSetField (tif, TIFFTAG_SAMPLEFORMAT,
                                            SAMPLEFORMAT_UINT);
                              TIFFSetField (tif, TIFFTAG_SAMPLESPERPIXEL, 4);
                              TIFFSetField (tif, TIFFTAG_BITSPERSAMPLE, 8);
                              TIFFSetField (tif, TIFFTAG_EXTRASAMPLES, 1,
                                            extra);
                              TIFFSetField (tif, TIFFTAG_PLANARCONFIG,
                                            PLANARCONFIG_CONTIG);
                              TIFFSetField (tif, TIFFTAG_PHOTOMETRIC,
                                            PHOTOMETRIC_RGB);
                              for (y = 0; y < image_height; y++)
                                {
                                  TIFFWriteScanline (
                                      tif, image + (y * image_width), y, 0);
                                }
                              TIFFFlush (tif);
                              TIFFClose (tif);
                              printf ("Saved image to file %s\n",
                                      image_file_name);
                            }
                        }
                    }
                  else
                    { /*key: s; toggle smudge on / off*/
                      Brush *brush = drawing->active_brushes;
                      while (brush != NULL)
                        {
                          brush->is_smudging = brush->is_smudging ? 0 : 1;
                          if (!brush->is_smudging)
                            {
                              brush->color = drawing->color;
                            }
                          brush = brush->next;
                        }
                    }
                  break;
                }
              case 56:
                { /*key: b; toggle paint on / off*/
                  drawing->is_drawing = drawing->is_drawing ? 0 : 1;
                  Brush *brush = drawing->active_brushes;
                  while (brush != NULL)
                    {
                      brush->is_drawing = brush->is_drawing ? 0 : 1;
                      brush = brush->next;
                    }
                  break;
                }
              case 26:
                { /*key: e; toggle erase on / off*/
                  Brush *brush = drawing->active_brushes;
                  while (brush != NULL)
                    {
                      brush->is_erasing = brush->is_erasing ? 0 : 1;
                      brush = brush->next;
                    }
                  break;
                }
              case 33:
                { /*key: p; toggle pick on / off*/
                  Brush *brush = drawing->active_brushes;
                  while (brush != NULL)
                    {
                      brush->is_picking = brush->is_picking ? 0 : 1;
                      brush = brush->next;
                    }
                  break;
                }
              case 46:
                { /*key: l; layer management*/
                  if (key_event->state & XCB_MOD_MASK_SHIFT)
                    { /*shift-l deletes topmost layer*/
                      del_top_layer (drawing);
                      if (drawing->current == NULL)
                        {
                          memset ((void *)image, 0x0,
                                  sizeof (uint32_t) * image_width
                                      * image_height);
                        }
                      rect invalid_area = { 0, 0, image_width, image_height };
                      update (drawing, invalid_area, image_width, image_height,
                              image, connection, window, draw, pixmap,
                              BACKGROUND);
                    }
                  else
                    {
                      add_top_layer (drawing, image_width, image_height);
                    }
                  break;
                }
              case 10:
                { /*key: 1; color number 1*/
                  colors_index = 0;
                  drawing->color = *Colors[colors_index];
                  Brush *brush = drawing->active_brushes;
                  while (brush != NULL)
                    {
                      brush->color = *Colors[colors_index];
                      brush = brush->next;
                    }
                  break;
                }
              case 11:
                { /*key: 2; color number 2*/
                  colors_index = 1;
                  drawing->color = *Colors[colors_index];
                  Brush *brush = drawing->active_brushes;
                  while (brush != NULL)
                    {
                      brush->color = *Colors[colors_index];
                      brush = brush->next;
                    }
                  break;
                }
              case 12:
                { /*key: 3; color number 3*/
                  colors_index = 2;
                  drawing->color = *Colors[colors_index];
                  Brush *brush = drawing->active_brushes;
                  while (brush != NULL)
                    {
                      brush->color = *Colors[colors_index];
                      brush = brush->next;
                    }
                  break;
                }
              case 13:
                { /*key: 4; color number 4*/
                  colors_index = 3;
                  drawing->color = *Colors[colors_index];
                  Brush *brush = drawing->active_brushes;
                  while (brush != NULL)
                    {
                      brush->color = *Colors[colors_index];
                      brush = brush->next;
                    }
                  break;
                }
              case 14:
                { /*key: 5; color number 5*/
                  colors_index = 4;
                  drawing->color = *Colors[colors_index];
                  Brush *brush = drawing->active_brushes;
                  while (brush != NULL)
                    {
                      brush->color = *Colors[colors_index];
                      brush = brush->next;
                    }
                  break;
                }
              case 15:
              case 16:
              case 17:
              case 18:
                { /*key: 6; color number 6*/
                  /*key: 7; color number 7*/
                  /*key: 8; color number 8*/
                  /*key: 9; color number 9*/
                  colors_index = key_event->detail - 10;
                  drawing->color = *Colors[colors_index];
                  Brush *brush = drawing->active_brushes;
                  while (brush != NULL)
                    {
                      brush->color = *Colors[colors_index];
                      brush = brush->next;
                    }
                  break;
                }
              case 54:
                { /*key: c; next color*/
                  if (colors_index >= 0)
                    {
                      if (key_event->state & XCB_MOD_MASK_SHIFT)
                        { /*shift-c previous color*/
                          colors_index -= 1;
                          if (colors_index < 0)
                            {
                              colors_index = colors - 1;
                            }
                        }
                      else
                        {
                          colors_index += 1;
                          if (colors_index >= colors)
                            {
                              colors_index = 0;
                            }
                        }
                      drawing->color = *Colors[colors_index];
                      Brush *brush = drawing->active_brushes;
                      while (brush != NULL)
                        {
                          brush->color = *Colors[colors_index];
                          brush = brush->next;
                        }
                    }
                  break;
                }
              case 58:
                { /*key: m; next blend mode*/
                  if (blend_mode >= 0)
                    {
                      if (key_event->state & XCB_MOD_MASK_SHIFT)
                        { /*shift-m previous blend mode*/
                          blend_mode -= 1;
                          if (blend_mode < 0)
                            {
                              blend_mode = BLEND_MODES - 1;
                            }
                        }
                      else
                        {
                          blend_mode += 1;
                          if (blend_mode >= colors)
                            {
                              blend_mode = 0;
                            }
                        }
                      Brush *brush = drawing->active_brushes;
                      while (brush != NULL)
                        {
                          brush->mode = blend_mode;
                          brush = brush->next;
                        }
                    }
                  break;
                }
              default:
                break;
              }
          }
        default:
          break;
        }
      free (event);
    }
  free (devices_reply);
  xcb_free_pixmap (connection, pixmap);
  xcb_disconnect (connection);
  free (image);

  while (drawing->bottom != NULL)
    {
      FloatingLayer *current = drawing->bottom;
      drawing->bottom = drawing->bottom->next;
      floating_layer_del (current);
    }

  return 0;
}
