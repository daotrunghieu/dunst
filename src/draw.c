#include "draw.h"

#include <assert.h>
#include <cairo.h>
#include <cairo-xlib.h>
#include <gdk/gdk.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib-object.h>
#include <locale.h>
#include <math.h>
#include <pango/pangocairo.h>
#include <pango/pango-attributes.h>
#include <pango/pango-font.h>
#include <pango/pango-layout.h>
#include <pango/pango-types.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "src/dunst.h"
#include "src/markup.h"
#include "src/notification.h"
#include "src/settings.h"
#include "src/utils.h"
#include "x11/x.h"

cairo_surface_t *cairo_surface;
cairo_t *cairo_context;
PangoFontDescription *pango_fdesc;

const char *color_strings[3][3];

struct geometry geometry;

typedef struct _colored_layout {
        PangoLayout *l;
        color_t fg;
        color_t bg;
        color_t frame;
        char *text;
        PangoAttrList *attr;
        cairo_surface_t *icon;
        notification *n;
} colored_layout;

void draw_setup()
{
        x_parse_geometry(&geometry);

        color_strings[ColFG][LOW] = settings.lowfgcolor;
        color_strings[ColFG][NORM] = settings.normfgcolor;
        color_strings[ColFG][CRIT] = settings.critfgcolor;

        color_strings[ColBG][LOW] = settings.lowbgcolor;
        color_strings[ColBG][NORM] = settings.normbgcolor;
        color_strings[ColBG][CRIT] = settings.critbgcolor;

        if (settings.lowframecolor)
                color_strings[ColFrame][LOW] = settings.lowframecolor;
        else
                color_strings[ColFrame][LOW] = settings.frame_color;
        if (settings.normframecolor)
                color_strings[ColFrame][NORM] = settings.normframecolor;
        else
                color_strings[ColFrame][NORM] = settings.frame_color;
        if (settings.critframecolor)
                color_strings[ColFrame][CRIT] = settings.critframecolor;
        else
                color_strings[ColFrame][CRIT] = settings.frame_color;

        x_setup();
        cairo_surface = x_cairo_create_surface();
        cairo_context = cairo_create(cairo_surface);
        pango_fdesc = pango_font_description_from_string(settings.font);
}

const struct geometry *draw_get_geometry()
{
        return &geometry;
}

void draw_free()
{
        cairo_surface_destroy(cairo_surface);
        cairo_destroy(cairo_context);
        x_free();
}

static color_t x_color_hex_to_double(int hexValue)
{
        color_t color;
        color.r = ((hexValue >> 16) & 0xFF) / 255.0;
        color.g = ((hexValue >> 8) & 0xFF) / 255.0;
        color.b = ((hexValue) & 0xFF) / 255.0;

        return color;
}

static color_t x_string_to_color_t(const char *str)
{
        char *end;
        long int val = strtol(str+1, &end, 16);
        if (*end != '\0' && *(end+1) != '\0') {
                printf("WARNING: Invalid color string: \"%s\"\n", str);
        }

        return x_color_hex_to_double(val);
}

static double _apply_delta(double base, double delta)
{
        base += delta;
        if (base > 1)
                base = 1;
        if (base < 0)
                base = 0;

        return base;
}

static color_t calculate_foreground_color(color_t bg)
{
        double c_delta = 0.1;
        color_t color = bg;

        /* do we need to darken or brighten the colors? */
        bool darken = (bg.r + bg.g + bg.b) / 3 > 0.5;

        int signedness = darken ? -1 : 1;

        color.r = _apply_delta(color.r, c_delta *signedness);
        color.g = _apply_delta(color.g, c_delta *signedness);
        color.b = _apply_delta(color.b, c_delta *signedness);

        return color;
}


static color_t x_get_separator_color(colored_layout *cl, colored_layout *cl_next)
{
        switch (settings.sep_color) {
                case FRAME:
                        if (cl_next->n->urgency > cl->n->urgency)
                                return cl_next->frame;
                        else
                                return cl->frame;
                case CUSTOM:
                        return x_string_to_color_t(settings.sep_custom_color_str);
                case FOREGROUND:
                        return cl->fg;
                case AUTO:
                        return calculate_foreground_color(cl->bg);
                default:
                        printf("Unknown separator color type. Please file a Bugreport.\n");
                        return cl->fg;

        }
}

static void r_setup_pango_layout(PangoLayout *layout, int width)
{
        pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
        pango_layout_set_width(layout, width * PANGO_SCALE);
        pango_layout_set_font_description(layout, pango_fdesc);
        pango_layout_set_spacing(layout, settings.line_height * PANGO_SCALE);

        PangoAlignment align;
        switch (settings.align) {
                case left:
                default:
                        align = PANGO_ALIGN_LEFT;
                        break;
                case center:
                        align = PANGO_ALIGN_CENTER;
                        break;
                case right:
                        align = PANGO_ALIGN_RIGHT;
                        break;
        }
        pango_layout_set_alignment(layout, align);

}

static void free_colored_layout(void *data)
{
        colored_layout *cl = data;
        g_object_unref(cl->l);
        pango_attr_list_unref(cl->attr);
        g_free(cl->text);
        if (cl->icon) cairo_surface_destroy(cl->icon);
        g_free(cl);
}

static bool does_file_exist(const char *filename){
        return (access(filename, F_OK) != -1);
}

static bool is_readable_file(const char *filename)
{
        return (access(filename, R_OK) != -1);
}

const char *get_filename_ext(const char *filename) {
        const char *dot = strrchr(filename, '.');
        if(!dot || dot == filename) return "";
        return dot + 1;
}

static dimension_t calculate_dimensions(GSList *layouts)
{
        dimension_t dim;
        dim.w = 0;
        dim.h = 0;
        dim.x = 0;
        dim.y = 0;

        screen_info *scr = get_active_screen();
        if (geometry.dynamic_width) {
                /* dynamic width */
                dim.w = 0;
        } else if (geometry.width != 0) {
                /* fixed width */
                if (geometry.negative_width) {
                        dim.w = scr->dim.w - geometry.width;
                } else {
                        dim.w = geometry.width;
                }
        } else {
                /* across the screen */
                dim.w = scr->dim.w;
        }

        dim.h += 2 * settings.frame_width;
        dim.h += (g_slist_length(layouts) - 1) * settings.separator_height;

        int text_width = 0, total_width = 0;
        for (GSList *iter = layouts; iter; iter = iter->next) {
                colored_layout *cl = iter->data;
                int w=0,h=0;
                pango_layout_get_pixel_size(cl->l, &w, &h);
                if (cl->icon) {
                        h = MAX(cairo_image_surface_get_height(cl->icon), h);
                        w += cairo_image_surface_get_width(cl->icon) + settings.h_padding;
                }
                h = MAX(settings.notification_height, h + settings.padding * 2);
                dim.h += h;
                text_width = MAX(w, text_width);

                if (geometry.dynamic_width || settings.shrink) {
                        /* dynamic width */
                        total_width = MAX(text_width + 2 * settings.h_padding, total_width);

                        /* subtract height from the unwrapped text */
                        dim.h -= h;

                        if (total_width > scr->dim.w) {
                                /* set width to screen width */
                                dim.w = scr->dim.w - geometry.x * 2;
                        } else if (geometry.dynamic_width || (total_width < geometry.width && settings.shrink)) {
                                /* set width to text width */
                                dim.w = total_width + 2 * settings.frame_width;
                        }

                        /* re-setup the layout */
                        w = dim.w;
                        w -= 2 * settings.h_padding;
                        w -= 2 * settings.frame_width;
                        if (cl->icon) w -= cairo_image_surface_get_width(cl->icon) + settings.h_padding;
                        r_setup_pango_layout(cl->l, w);

                        /* re-read information */
                        pango_layout_get_pixel_size(cl->l, &w, &h);
                        if (cl->icon) {
                                h = MAX(cairo_image_surface_get_height(cl->icon), h);
                                w += cairo_image_surface_get_width(cl->icon) + settings.h_padding;
                        }
                        h = MAX(settings.notification_height, h + settings.padding * 2);
                        dim.h += h;
                        text_width = MAX(w, text_width);
                }
        }

        if (dim.w <= 0) {
                dim.w = text_width + 2 * settings.h_padding;
                dim.w += 2 * settings.frame_width;
        }

        return dim;
}

static cairo_surface_t *gdk_pixbuf_to_cairo_surface(const GdkPixbuf *pixbuf)
{
        cairo_surface_t *icon_surface = NULL;
        cairo_t *cr;
        cairo_format_t format;
        double width, height;

        format = gdk_pixbuf_get_has_alpha(pixbuf) ? CAIRO_FORMAT_ARGB32 : CAIRO_FORMAT_RGB24;
        width = gdk_pixbuf_get_width(pixbuf);
        height = gdk_pixbuf_get_height(pixbuf);
        icon_surface = cairo_image_surface_create(format, width, height);
        cr = cairo_create(icon_surface);
        gdk_cairo_set_source_pixbuf(cr, pixbuf, 0, 0);
        cairo_paint(cr);
        cairo_destroy(cr);
        return icon_surface;
}

static GdkPixbuf *get_pixbuf_from_file(const char *icon_path)
{
        GdkPixbuf *pixbuf = NULL;
        if (is_readable_file(icon_path)) {
                GError *error = NULL;
                pixbuf = gdk_pixbuf_new_from_file(icon_path, &error);
                if (pixbuf == NULL)
                        g_free(error);
        }
        return pixbuf;
}

static GdkPixbuf *get_pixbuf_from_path(char *icon_path)
{
        GdkPixbuf *pixbuf = NULL;
        gchar *uri_path = NULL;
        if (strlen(icon_path) > 0) {
                if (g_str_has_prefix(icon_path, "file://")) {
                        uri_path = g_filename_from_uri(icon_path, NULL, NULL);
                        if (uri_path != NULL) {
                                icon_path = uri_path;
                        }
                }
                /* absolute path? */
                if (icon_path[0] == '/' || icon_path[0] == '~') {
                        pixbuf = get_pixbuf_from_file(icon_path);
                }
                /* search in icon_path */
                if (pixbuf == NULL) {
                        char *start = settings.icon_path,
                             *end, *current_folder, *maybe_icon_path;
                        do {
                                end = strchr(start, ':');
                                if (end == NULL) end = strchr(settings.icon_path, '\0'); /* end = end of string */

                                current_folder = g_strndup(start, end - start);
                                /* try svg */
                                maybe_icon_path = g_strconcat(current_folder, "/", icon_path, ".svg", NULL);
                                if (!does_file_exist(maybe_icon_path)) {
                                        g_free(maybe_icon_path);
                                        /* fallback to png */
                                        maybe_icon_path = g_strconcat(current_folder, "/", icon_path, ".png", NULL);
                                }
                                g_free(current_folder);

                                pixbuf = get_pixbuf_from_file(maybe_icon_path);
                                g_free(maybe_icon_path);
                                if (pixbuf != NULL) {
                                        return pixbuf;
                                }

                                start = end + 1;
                        } while (*(end) != '\0');
                }
                if (pixbuf == NULL) {
                        fprintf(stderr,
                                "Could not load icon: '%s'\n", icon_path);
                }
                if (uri_path != NULL) {
                        g_free(uri_path);
                }
        }
        return pixbuf;
}

static GdkPixbuf *get_pixbuf_from_raw_image(const RawImage *raw_image)
{
        GdkPixbuf *pixbuf = NULL;

        pixbuf = gdk_pixbuf_new_from_data(raw_image->data,
                                          GDK_COLORSPACE_RGB,
                                          raw_image->has_alpha,
                                          raw_image->bits_per_sample,
                                          raw_image->width,
                                          raw_image->height,
                                          raw_image->rowstride,
                                          NULL,
                                          NULL);

        return pixbuf;
}

static PangoLayout *create_layout(cairo_t *c)
{
        screen_info *screen = get_active_screen();

        PangoContext *context = pango_cairo_create_context(c);
        pango_cairo_context_set_resolution(context, get_dpi_for_screen(screen));

        PangoLayout *layout = pango_layout_new(context);

        g_object_unref(context);

        return layout;
}

static colored_layout *r_init_shared(cairo_t *c, notification *n)
{
        colored_layout *cl = g_malloc(sizeof(colored_layout));
        cl->l = create_layout(c);

        if (!settings.word_wrap) {
                PangoEllipsizeMode ellipsize;
                switch (settings.ellipsize) {
                        case start:
                                ellipsize = PANGO_ELLIPSIZE_START;
                                break;
                        case middle:
                                ellipsize = PANGO_ELLIPSIZE_MIDDLE;
                                break;
                        case end:
                                ellipsize = PANGO_ELLIPSIZE_END;
                                break;
                        default:
                                assert(false);
                }
                pango_layout_set_ellipsize(cl->l, ellipsize);
        }

        GdkPixbuf *pixbuf = NULL;

        if (n->raw_icon && !n->icon_overridden &&
            settings.icon_position != icons_off) {

                pixbuf = get_pixbuf_from_raw_image(n->raw_icon);

        } else if (n->icon && settings.icon_position != icons_off) {
                pixbuf = get_pixbuf_from_path(n->icon);
        }

        if (pixbuf != NULL) {
                int w = gdk_pixbuf_get_width(pixbuf);
                int h = gdk_pixbuf_get_height(pixbuf);
                int larger = w > h ? w : h;
                if (settings.max_icon_size && larger > settings.max_icon_size) {
                        GdkPixbuf *scaled;
                        if (w >= h) {
                                scaled = gdk_pixbuf_scale_simple(pixbuf,
                                                settings.max_icon_size,
                                                (int) ((double) settings.max_icon_size / w * h),
                                                GDK_INTERP_BILINEAR);
                        } else {
                                scaled = gdk_pixbuf_scale_simple(pixbuf,
                                                (int) ((double) settings.max_icon_size / h * w),
                                                settings.max_icon_size,
                                                GDK_INTERP_BILINEAR);
                        }
                        g_object_unref(pixbuf);
                        pixbuf = scaled;
                }

                cl->icon = gdk_pixbuf_to_cairo_surface(pixbuf);
                g_object_unref(pixbuf);
        } else {
                cl->icon = NULL;
        }

        if (cl->icon && cairo_surface_status(cl->icon) != CAIRO_STATUS_SUCCESS) {
                cairo_surface_destroy(cl->icon);
                cl->icon = NULL;
        }

        cl->fg = x_string_to_color_t(n->color_strings[ColFG]);
        cl->bg = x_string_to_color_t(n->color_strings[ColBG]);
        cl->frame = x_string_to_color_t(n->color_strings[ColFrame]);

        cl->n = n;

        dimension_t dim = calculate_dimensions(NULL);
        int width = dim.w;

        if (geometry.dynamic_width) {
                r_setup_pango_layout(cl->l, -1);
        } else {
                width -= 2 * settings.h_padding;
                width -= 2 * settings.frame_width;
                if (cl->icon) width -= cairo_image_surface_get_width(cl->icon) + settings.h_padding;
                r_setup_pango_layout(cl->l, width);
        }

        return cl;
}

static colored_layout *r_create_layout_for_xmore(cairo_t *c, notification *n, int qlen)
{
       colored_layout *cl = r_init_shared(c, n);
       cl->text = g_strdup_printf("(%d more)", qlen);
       cl->attr = NULL;
       pango_layout_set_text(cl->l, cl->text, -1);
       return cl;
}

static colored_layout *r_create_layout_from_notification(cairo_t *c, notification *n)
{

        colored_layout *cl = r_init_shared(c, n);

        /* markup */
        GError *err = NULL;
        pango_parse_markup(n->text_to_render, -1, 0, &(cl->attr), &(cl->text), NULL, &err);

        if (!err) {
                pango_layout_set_text(cl->l, cl->text, -1);
                pango_layout_set_attributes(cl->l, cl->attr);
        } else {
                /* remove markup and display plain message instead */
                n->text_to_render = markup_strip(n->text_to_render);
                cl->text = NULL;
                cl->attr = NULL;
                pango_layout_set_text(cl->l, n->text_to_render, -1);
                if (n->first_render) {
                        printf("Error parsing markup: %s\n", err->message);
                }
                g_error_free(err);
        }


        pango_layout_get_pixel_size(cl->l, NULL, &(n->displayed_height));
        if (cl->icon) n->displayed_height = MAX(cairo_image_surface_get_height(cl->icon), n->displayed_height);
        n->displayed_height = MAX(settings.notification_height, n->displayed_height + settings.padding * 2);

        n->first_render = false;
        return cl;
}

static GSList *r_create_layouts(cairo_t *c)
{
        GSList *layouts = NULL;

        int qlen = g_list_length(g_queue_peek_head_link(queue));
        bool xmore_is_needed = qlen > 0 && settings.indicate_hidden;

        notification *last = NULL;
        for (GList *iter = g_queue_peek_head_link(displayed);
                        iter; iter = iter->next)
        {
                notification *n = iter->data;
                last = n;

                notification_update_text_to_render(n);

                if (!iter->next && xmore_is_needed && geometry.height == 1) {
                        char *new_ttr = g_strdup_printf("%s (%d more)", n->text_to_render, qlen);
                        g_free(n->text_to_render);
                        n->text_to_render = new_ttr;
                }
                layouts = g_slist_append(layouts,
                                r_create_layout_from_notification(c, n));
        }

        if (xmore_is_needed && geometry.height != 1) {
                /* append xmore message as new message */
                layouts = g_slist_append(layouts,
                        r_create_layout_for_xmore(c, last, qlen));
        }

        return layouts;
}

static void r_free_layouts(GSList *layouts)
{
        g_slist_free_full(layouts, free_colored_layout);
}

static dimension_t x_render_layout(cairo_t *c, colored_layout *cl, colored_layout *cl_next, dimension_t dim, bool first, bool last)
{
        int h;
        int h_text = 0;
        pango_layout_get_pixel_size(cl->l, NULL, &h);
        if (cl->icon) {
                h_text = h;
                h = MAX(cairo_image_surface_get_height(cl->icon), h);
        }

        int bg_x = 0;
        int bg_y = dim.y;
        int bg_width = dim.w;
        int bg_height = MAX(settings.notification_height, (2 * settings.padding) + h);
        double bg_half_height = settings.notification_height/2.0;
        int pango_offset = (int) floor(h/2.0);

        if (first) bg_height += settings.frame_width;
        if (last) bg_height += settings.frame_width;
        else bg_height += settings.separator_height;

        cairo_set_source_rgb(c, cl->frame.r, cl->frame.g, cl->frame.b);
        cairo_rectangle(c, bg_x, bg_y, bg_width, bg_height);
        cairo_fill(c);

        /* adding frame */
        bg_x += settings.frame_width;
        if (first) {
                dim.y += settings.frame_width;
                bg_y += settings.frame_width;
                bg_height -= settings.frame_width;
                if (!last) bg_height -= settings.separator_height;
        }
        bg_width -= 2 * settings.frame_width;
        if (last)
                bg_height -= settings.frame_width;

        cairo_set_source_rgb(c, cl->bg.r, cl->bg.g, cl->bg.b);
        cairo_rectangle(c, bg_x, bg_y, bg_width, bg_height);
        cairo_fill(c);

        bool use_padding = settings.notification_height <= (2 * settings.padding) + h;
        if (use_padding)
            dim.y += settings.padding;
        else
            dim.y += (int) (ceil(bg_half_height) - pango_offset);

        if (cl->icon && settings.icon_position == icons_left) {
                cairo_move_to(c, settings.frame_width + cairo_image_surface_get_width(cl->icon) + 2 * settings.h_padding, bg_y + settings.padding + h/2 - h_text/2);
        } else if (cl->icon && settings.icon_position == icons_right) {
                cairo_move_to(c, settings.frame_width + settings.h_padding, bg_y + settings.padding + h/2 - h_text/2);
        } else {
                cairo_move_to(c, settings.frame_width + settings.h_padding, bg_y + settings.padding);
        }

        cairo_set_source_rgb(c, cl->fg.r, cl->fg.g, cl->fg.b);
        pango_cairo_update_layout(c, cl->l);
        pango_cairo_show_layout(c, cl->l);
        if (use_padding)
            dim.y += h + settings.padding;
        else
            dim.y += (int) (floor(bg_half_height) + pango_offset);

        if (settings.separator_height > 0 && !last) {
                color_t sep_color = x_get_separator_color(cl, cl_next);
                cairo_set_source_rgb(c, sep_color.r, sep_color.g, sep_color.b);

                if (settings.sep_color == FRAME)
                        // Draw over the borders on both sides to avoid
                        // the wrong color in the corners.
                        cairo_rectangle(c, 0, dim.y, dim.w, settings.separator_height);
                else
                        cairo_rectangle(c, settings.frame_width, dim.y + settings.frame_width, dim.w - 2 * settings.frame_width, settings.separator_height);

                cairo_fill(c);
                dim.y += settings.separator_height;
        }
        cairo_move_to(c, settings.h_padding, dim.y);

        if (cl->icon)  {
                unsigned int image_width = cairo_image_surface_get_width(cl->icon),
                             image_height = cairo_image_surface_get_height(cl->icon),
                             image_x,
                             image_y = bg_y + settings.padding + h/2 - image_height/2;

                if (settings.icon_position == icons_left) {
                        image_x = settings.frame_width + settings.h_padding;
                } else {
                        image_x = bg_width - settings.h_padding - image_width + settings.frame_width;
                }

                cairo_set_source_surface (c, cl->icon, image_x, image_y);
                cairo_rectangle (c, image_x, image_y, image_width, image_height);
                cairo_fill (c);
        }

        return dim;
}

void draw()
{
        GSList *layouts = r_create_layouts(cairo_context);

        dimension_t dim = calculate_dimensions(layouts);
        int width = dim.w;
        int height = dim.h;

        cairo_t *c;
        cairo_surface_t *image_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
        c = cairo_create(image_surface);

        x_win_move(width, height);
        cairo_xlib_surface_set_size(cairo_surface, width, height);

        cairo_move_to(c, 0, 0);

        bool first = true;
        for (GSList *iter = layouts; iter; iter = iter->next) {
                if (iter->next)
                        dim = x_render_layout(c, iter->data, iter->next->data, dim, first, iter->next == NULL);
                else
                        dim = x_render_layout(c, iter->data, NULL, dim, first, iter->next == NULL);

                first = false;
        }

        cairo_set_source_surface(cairo_context, image_surface, 0, 0);
        cairo_paint(cairo_context);
        cairo_show_page(cairo_context);

        cairo_destroy(c);
        cairo_surface_destroy(image_surface);
        r_free_layouts(layouts);
}

/* vim: set tabstop=8 shiftwidth=8 expandtab textwidth=0: */
