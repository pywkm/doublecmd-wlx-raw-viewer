/*
 * rawview - Double Commander viewer (WLX) plugin for camera RAW files, GTK2.
 *
 * The R key or a double click cycles through three sources; the choice is
 * kept for the following files:
 *
 *   1. the JPEG preview embedded in the file (default; fast, and for DNGs it
 *      carries the look developed in Lightroom, but it is often small),
 *   2. a full-resolution decode of the sensor data (neutral look),
 *   3. experimental: sensor detail with the look of the embedded preview.
 *
 * The image is always scaled to fit the viewer window.
 */
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <libraw/libraw.h>
#include <math.h>
#include <string.h>

#define LISTPLUGIN_OK    0
#define LISTPLUGIN_ERROR 1

/* Demosaic algorithm for libraw: 2 = PPG, a good speed/quality trade-off. */
#define DEMOSAIC_QUALITY 2

#define DETECT_STRING \
    "EXT=\"NEF\" | EXT=\"NRW\" | EXT=\"DNG\" | EXT=\"CR2\" | EXT=\"CR3\" | " \
    "EXT=\"CRW\" | EXT=\"ARW\" | EXT=\"SR2\" | EXT=\"SRF\" | EXT=\"RAF\" | " \
    "EXT=\"ORF\" | EXT=\"RW2\" | EXT=\"PEF\" | EXT=\"SRW\" | EXT=\"X3F\" | " \
    "EXT=\"ERF\" | EXT=\"KDC\" | EXT=\"DCR\" | EXT=\"MRW\" | EXT=\"3FR\" | " \
    "EXT=\"IIQ\" | EXT=\"RWL\" | EXT=\"RAW\""

#define DATA_KEY "rawview-state"

typedef enum {
    MODE_EMBEDDED,
    MODE_SENSOR,
    MODE_TRANSFER,
    MODE_COUNT
} ViewMode;

typedef struct {
    GdkPixbuf *original;
    GdkPixbuf *scaled;   /* cache of `original` fitted to the last allocation */
    gchar *filename;
    const char *label;   /* what is being shown; a static string */
} ViewState;

/* Shared by all viewer windows so the choice survives moving between files. */
static ViewMode view_mode = MODE_EMBEDDED;

static GdkPixbuf *rotate_by_flip(GdkPixbuf *pixbuf, int flip)
{
    GdkPixbufRotation rotation;

    switch (flip) {
    case 3: rotation = GDK_PIXBUF_ROTATE_UPSIDEDOWN; break;
    case 5: rotation = GDK_PIXBUF_ROTATE_COUNTERCLOCKWISE; break;
    case 6: rotation = GDK_PIXBUF_ROTATE_CLOCKWISE; break;
    default: return pixbuf;
    }

    GdkPixbuf *rotated = gdk_pixbuf_rotate_simple(pixbuf, rotation);
    if (!rotated)
        return pixbuf;
    g_object_unref(pixbuf);
    return rotated;
}

static GdkPixbuf *pixbuf_from_jpeg(const guchar *data, gsize size)
{
    GdkPixbufLoader *loader = gdk_pixbuf_loader_new();
    GdkPixbuf *pixbuf = NULL;

    if (gdk_pixbuf_loader_write(loader, data, size, NULL) &&
        gdk_pixbuf_loader_close(loader, NULL)) {
        pixbuf = gdk_pixbuf_loader_get_pixbuf(loader);
        if (pixbuf)
            g_object_ref(pixbuf);
    } else {
        gdk_pixbuf_loader_close(loader, NULL);
    }
    g_object_unref(loader);
    return pixbuf;
}

static GdkPixbuf *pixbuf_from_processed(libraw_processed_image_t *image)
{
    if (image->type == LIBRAW_IMAGE_JPEG)
        return pixbuf_from_jpeg(image->data, image->data_size);

    if (image->type != LIBRAW_IMAGE_BITMAP || image->colors != 3 || image->bits != 8)
        return NULL;

    /* Copy, because libraw's rows are tightly packed and the buffer is freed. */
    GdkPixbuf *pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8,
                                       image->width, image->height);
    if (!pixbuf)
        return NULL;

    guchar *pixels = gdk_pixbuf_get_pixels(pixbuf);
    int rowstride = gdk_pixbuf_get_rowstride(pixbuf);
    for (int y = 0; y < image->height; y++)
        memcpy(pixels + (gsize)y * rowstride,
               image->data + (gsize)y * image->width * 3,
               (gsize)image->width * 3);
    return pixbuf;
}

static GdkPixbuf *load_embedded_preview(libraw_data_t *raw)
{
    int error = 0;

    if (libraw_unpack_thumb(raw) != LIBRAW_SUCCESS)
        return NULL;

    libraw_processed_image_t *thumb = libraw_dcraw_make_mem_thumb(raw, &error);
    if (!thumb)
        return NULL;

    gboolean is_jpeg = thumb->type == LIBRAW_IMAGE_JPEG;
    GdkPixbuf *pixbuf = pixbuf_from_processed(thumb);
    libraw_dcraw_clear_mem(thumb);
    if (!pixbuf)
        return NULL;

    /* Prefer the preview's own EXIF orientation; otherwise use the RAW's. */
    const gchar *orientation = is_jpeg ? gdk_pixbuf_get_option(pixbuf, "orientation") : NULL;
    if (orientation && strcmp(orientation, "1") != 0) {
        GdkPixbuf *oriented = gdk_pixbuf_apply_embedded_orientation(pixbuf);
        if (oriented) {
            g_object_unref(pixbuf);
            pixbuf = oriented;
        }
    } else {
        pixbuf = rotate_by_flip(pixbuf, raw->sizes.flip);
    }
    return pixbuf;
}

static GdkPixbuf *load_sensor_decode(libraw_data_t *raw)
{
    int error = 0;

    raw->params.user_qual = DEMOSAIC_QUALITY;
    raw->params.use_camera_wb = 1;
    raw->params.output_bps = 8;

    if (libraw_unpack(raw) != LIBRAW_SUCCESS ||
        libraw_dcraw_process(raw) != LIBRAW_SUCCESS)
        return NULL;

    libraw_processed_image_t *image = libraw_dcraw_make_mem_image(raw, &error);
    if (!image)
        return NULL;

    /* libraw already applied the RAW's orientation while processing. */
    GdkPixbuf *pixbuf = pixbuf_from_processed(image);
    libraw_dcraw_clear_mem(image);
    return pixbuf;
}

/* Mean over a (2 * radius + 1) window, shrunk at the borders. */
static void box_filter(const float *source, float *target, float *scratch,
                       int width, int height, int radius)
{
    for (int y = 0; y < height; y++) {
        const float *row = source + (gsize)y * width;
        float *out = scratch + (gsize)y * width;
        double sum = 0;
        int count = 0;

        for (int x = 0; x < MIN(radius, width); x++, count++)
            sum += row[x];
        for (int x = 0; x < width; x++) {
            if (x + radius < width) {
                sum += row[x + radius];
                count++;
            }
            if (x - radius - 1 >= 0) {
                sum -= row[x - radius - 1];
                count--;
            }
            out[x] = sum / count;
        }
    }

    for (int x = 0; x < width; x++) {
        double sum = 0;
        int count = 0;

        for (int y = 0; y < MIN(radius, height); y++, count++)
            sum += scratch[(gsize)y * width + x];
        for (int y = 0; y < height; y++) {
            if (y + radius < height) {
                sum += scratch[(gsize)(y + radius) * width + x];
                count++;
            }
            if (y - radius - 1 >= 0) {
                sum -= scratch[(gsize)(y - radius - 1) * width + x];
                count--;
            }
            target[(gsize)y * width + x] = sum / count;
        }
    }
}

static inline float luminance(const guchar *pixel)
{
    return (0.299f * pixel[0] + 0.587f * pixel[1] + 0.114f * pixel[2]) / 255.0f;
}

/*
 * Gives `decoded` the look of `preview`, in place. The preview supplies colour
 * and tone; the decode supplies only the detail the small preview lacks, i.e.
 * the difference between the full-resolution decode and the same decode
 * reduced to the preview's size. That detail is scaled by `a`, the local slope
 * of  preview_luminance = a * decode_luminance + b  fitted at the preview's
 * resolution, so it follows the preview's tone curve. Where the two images do
 * not match locally, `a` tends to 0 and the output is the plain upscaled
 * preview.
 *
 * Returns FALSE when the images clearly show different frames.
 */
static gboolean transfer_look(GdkPixbuf *decoded, GdkPixbuf *preview)
{
    int dw = gdk_pixbuf_get_width(decoded), dh = gdk_pixbuf_get_height(decoded);
    int pw = gdk_pixbuf_get_width(preview), ph = gdk_pixbuf_get_height(preview);
    int dstride = gdk_pixbuf_get_rowstride(decoded);
    int pstride = gdk_pixbuf_get_rowstride(preview);
    int pchannels = gdk_pixbuf_get_n_channels(preview);
    guchar *dpixels = gdk_pixbuf_get_pixels(decoded);
    const guchar *ppixels = gdk_pixbuf_get_pixels(preview);

    if (gdk_pixbuf_get_n_channels(decoded) != 3 || pw > dw || ph > dh || pw < 16 || ph < 16)
        return FALSE;

    double decoded_aspect = (double)dw / dh;
    if (fabs((double)pw / ph - decoded_aspect) > 0.02 * decoded_aspect)
        return FALSE;

    gsize count = (gsize)pw * ph;
    float *guide = g_new0(float, count);       /* decode luminance, preview size */
    float *target = g_new(float, count);       /* preview luminance */
    float *guide_guide = g_new(float, count);
    float *guide_target = g_new(float, count);
    float *slope = g_new(float, count);        /* a */
    float *offset = g_new(float, count);       /* mean of target, then b */
    float *scratch = g_new(float, count);
    guint32 *weights = g_new0(guint32, count);
    int *bin_x = g_new(int, dw);
    gboolean matched = FALSE;

    for (int x = 0; x < dw; x++)
        bin_x[x] = (int)((gint64)x * pw / dw);

    /* Area-average the decode's luminance down to the preview's size. */
    for (int y = 0; y < dh; y++) {
        const guchar *row = dpixels + (gsize)y * dstride;
        gsize bin_row = (gsize)((gint64)y * ph / dh) * pw;

        for (int x = 0; x < dw; x++) {
            guide[bin_row + bin_x[x]] += luminance(row + x * 3);
            weights[bin_row + bin_x[x]]++;
        }
    }

    double sum_guide = 0, sum_target = 0;
    for (int y = 0; y < ph; y++) {
        for (int x = 0; x < pw; x++) {
            gsize i = (gsize)y * pw + x;

            guide[i] /= MAX(weights[i], 1);
            target[i] = luminance(ppixels + (gsize)y * pstride + x * pchannels);
            sum_guide += guide[i];
            sum_target += target[i];
        }
    }

    /* A weak overall correlation means the two images are not the same frame. */
    double mean_guide = sum_guide / count, mean_target = sum_target / count;
    double covariance = 0, variance_guide = 0, variance_target = 0;
    for (gsize i = 0; i < count; i++) {
        double g = guide[i] - mean_guide, t = target[i] - mean_target;

        covariance += g * t;
        variance_guide += g * g;
        variance_target += t * t;
        guide_guide[i] = guide[i] * guide[i];
        guide_target[i] = guide[i] * target[i];
    }
    if (variance_guide <= 0 || variance_target <= 0 ||
        covariance / sqrt(variance_guide * variance_target) < 0.5)
        goto out;

    int radius = MAX(2, pw / 256);
    const float epsilon = 4e-4f;

    box_filter(guide_guide, guide_guide, scratch, pw, ph, radius);
    box_filter(guide_target, guide_target, scratch, pw, ph, radius);
    box_filter(guide, slope, scratch, pw, ph, radius);    /* mean of guide */
    box_filter(target, offset, scratch, pw, ph, radius);  /* mean of target */

    for (gsize i = 0; i < count; i++) {
        float mean_g = slope[i], mean_t = offset[i];
        float a = (guide_target[i] - mean_g * mean_t) /
                  (guide_guide[i] - mean_g * mean_g + epsilon);

        slope[i] = CLAMP(a, 0.0f, 4.0f);
    }
    box_filter(slope, slope, scratch, pw, ph, radius);

    /* Bilinear sampling positions of the preview grid for each decode column. */
    int *x0 = g_new(int, dw), *x1 = g_new(int, dw);
    float *wx = g_new(float, dw);
    for (int x = 0; x < dw; x++) {
        float fx = (x + 0.5f) * pw / dw - 0.5f;

        fx = CLAMP(fx, 0.0f, pw - 1.0f);
        x0[x] = (int)fx;
        x1[x] = MIN(x0[x] + 1, pw - 1);
        wx[x] = fx - x0[x];
    }

    for (int y = 0; y < dh; y++) {
        float fy = (y + 0.5f) * ph / dh - 0.5f;

        fy = CLAMP(fy, 0.0f, ph - 1.0f);
        int y0 = (int)fy, y1 = MIN(y0 + 1, ph - 1);
        float wy = fy - y0;
        const guchar *prow0 = ppixels + (gsize)y0 * pstride;
        const guchar *prow1 = ppixels + (gsize)y1 * pstride;
        const float *arow0 = slope + (gsize)y0 * pw, *arow1 = slope + (gsize)y1 * pw;
        const float *grow0 = guide + (gsize)y0 * pw, *grow1 = guide + (gsize)y1 * pw;
        guchar *drow = dpixels + (gsize)y * dstride;

        for (int x = 0; x < dw; x++) {
            int left = x0[x], right = x1[x];
            float w11 = wx[x] * wy, w10 = wx[x] - w11;
            float w01 = wy - w11, w00 = 1.0f - wx[x] - w01;
            guchar *pixel = drow + x * 3;
            float look[3];

            for (int c = 0; c < 3; c++)
                look[c] = w00 * prow0[left * pchannels + c] + w10 * prow0[right * pchannels + c] +
                          w01 * prow1[left * pchannels + c] + w11 * prow1[right * pchannels + c];

            float a = w00 * arow0[left] + w10 * arow0[right] + w01 * arow1[left] + w11 * arow1[right];
            float reduced = w00 * grow0[left] + w10 * grow0[right] + w01 * grow1[left] + w11 * grow1[right];
            float smooth = (0.299f * look[0] + 0.587f * look[1] + 0.114f * look[2]) / 255.0f;
            float detailed = smooth + a * (luminance(pixel) - reduced);
            float gain = (detailed + 0.03f) / (smooth + 0.03f);

            gain = CLAMP(gain, 0.5f, 2.0f);
            for (int c = 0; c < 3; c++) {
                float value = look[c] * gain + 0.5f;

                pixel[c] = (guchar)CLAMP(value, 0.0f, 255.0f);
            }
        }
    }

    g_free(x0);
    g_free(x1);
    g_free(wx);
    matched = TRUE;

out:
    g_free(guide);
    g_free(target);
    g_free(guide_guide);
    g_free(guide_target);
    g_free(slope);
    g_free(offset);
    g_free(scratch);
    g_free(weights);
    g_free(bin_x);
    return matched;
}

static GdkPixbuf *load_with_preview_look(libraw_data_t *raw, const char **label)
{
    GdkPixbuf *preview = load_embedded_preview(raw);

    if (!preview) {
        *label = "sensor decode (no embedded preview)";
        return load_sensor_decode(raw);
    }

    *label = "embedded preview (already full size)";
    if (MAX(gdk_pixbuf_get_width(preview), gdk_pixbuf_get_height(preview)) * 5 >=
        MAX(raw->sizes.width, raw->sizes.height) * 4)
        return preview;

    /*
     * The decode is the uncropped frame. A preview rendered after cropping in
     * Lightroom shows a different frame, which transfer_look() rejects.
     */
    *label = "embedded preview (sensor detail did not match)";
    GdkPixbuf *decoded = load_sensor_decode(raw);
    if (!decoded)
        return preview;

    if (!transfer_look(decoded, preview)) {
        g_object_unref(decoded);
        return preview;
    }

    g_object_unref(preview);
    *label = "sensor detail + preview look";
    return decoded;
}

static GdkPixbuf *load_raw(const char *filename, const char **label)
{
    libraw_data_t *raw = libraw_init(0);
    if (!raw)
        return NULL;

    GdkPixbuf *pixbuf = NULL;
    if (libraw_open_file(raw, filename) == LIBRAW_SUCCESS) {
        switch (view_mode) {
        case MODE_TRANSFER:
            pixbuf = load_with_preview_look(raw, label);
            break;
        case MODE_SENSOR:
            *label = "sensor decode";
            pixbuf = load_sensor_decode(raw);
            if (!pixbuf) {
                *label = "embedded preview (sensor decode failed)";
                pixbuf = load_embedded_preview(raw);
            }
            break;
        default:
            *label = "embedded preview";
            pixbuf = load_embedded_preview(raw);
            if (!pixbuf) {
                *label = "sensor decode (no embedded preview)";
                pixbuf = load_sensor_decode(raw);
            }
            break;
        }
    }
    libraw_close(raw);
    return pixbuf;
}

static void view_state_free(gpointer data)
{
    ViewState *state = data;

    if (state->original)
        g_object_unref(state->original);
    if (state->scaled)
        g_object_unref(state->scaled);
    g_free(state->filename);
    g_free(state);
}

static void view_state_set(ViewState *state, GdkPixbuf *pixbuf, const char *filename,
                           const char *label)
{
    if (state->filename != filename) {
        g_free(state->filename);
        state->filename = g_strdup(filename);
    }
    if (state->original)
        g_object_unref(state->original);
    if (state->scaled)
        g_object_unref(state->scaled);
    state->original = pixbuf;
    state->scaled = NULL;
    state->label = label;
}

static void draw_label(cairo_t *cr, const char *label, int height)
{
    cairo_text_extents_t extents;

    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 14);
    cairo_text_extents(cr, label, &extents);

    cairo_set_source_rgba(cr, 0, 0, 0, 0.55);
    cairo_rectangle(cr, 8, height - 34, extents.x_advance + 16, 26);
    cairo_fill(cr);

    cairo_set_source_rgb(cr, 0.92, 0.92, 0.92);
    cairo_move_to(cr, 16, height - 16);
    cairo_show_text(cr, label);
}

static gboolean on_expose(GtkWidget *widget, GdkEventExpose *event, gpointer data)
{
    ViewState *state = data;
    GtkAllocation allocation;

    gtk_widget_get_allocation(widget, &allocation);

    cairo_t *cr = gdk_cairo_create(gtk_widget_get_window(widget));
    gdk_cairo_region(cr, event->region);
    cairo_clip(cr);
    cairo_set_source_rgb(cr, 0.12, 0.12, 0.12);
    cairo_paint(cr);

    if (state->original && allocation.width > 1 && allocation.height > 1) {
        int width = gdk_pixbuf_get_width(state->original);
        int height = gdk_pixbuf_get_height(state->original);
        double scale = MIN((double)allocation.width / width,
                           (double)allocation.height / height);

        int target_width = MAX(1, (int)(width * scale + 0.5));
        int target_height = MAX(1, (int)(height * scale + 0.5));

        if (!state->scaled ||
            gdk_pixbuf_get_width(state->scaled) != target_width ||
            gdk_pixbuf_get_height(state->scaled) != target_height) {
            if (state->scaled)
                g_object_unref(state->scaled);
            state->scaled = gdk_pixbuf_scale_simple(state->original, target_width,
                                                    target_height, GDK_INTERP_BILINEAR);
        }

        if (state->scaled) {
            gdk_cairo_set_source_pixbuf(cr, state->scaled,
                                        (allocation.width - target_width) / 2,
                                        (allocation.height - target_height) / 2);
            cairo_paint(cr);
        }

        if (state->label)
            draw_label(cr, state->label, allocation.height);
    }

    cairo_destroy(cr);
    return TRUE;
}

static void cycle_source(GtkWidget *area)
{
    ViewState *state = g_object_get_data(G_OBJECT(area), DATA_KEY);
    const char *label = NULL;

    view_mode = (view_mode + 1) % MODE_COUNT;
    GdkPixbuf *pixbuf = load_raw(state->filename, &label);
    if (pixbuf) {
        view_state_set(state, pixbuf, state->filename, label);
        gtk_widget_queue_draw(area);
    }
}

static gboolean on_button_press(GtkWidget *widget, GdkEventButton *event, gpointer data)
{
    (void)data;

    if (event->type != GDK_2BUTTON_PRESS || event->button != 1)
        return FALSE;

    cycle_source(widget);
    return TRUE;
}

/*
 * The plugin widget never has keyboard focus (DC keeps it so its own viewer
 * hotkeys work), so the key is caught on the toplevel window. In Quick View
 * that toplevel is DC's main window, where a plain letter must stay available
 * for typing; it is recognised by its title.
 */
static gboolean on_toplevel_key_press(GtkWidget *toplevel, GdkEventKey *event, gpointer data)
{
    GtkWidget *area = data;

    if (event->keyval != GDK_r && event->keyval != GDK_R)
        return FALSE;
    if (event->state & (GDK_CONTROL_MASK | GDK_MOD1_MASK | GDK_SUPER_MASK))
        return FALSE;

    const gchar *title = gtk_window_get_title(GTK_WINDOW(toplevel));
    if (title && g_str_has_prefix(title, "Double Commander"))
        return FALSE;

    cycle_source(area);
    return TRUE;
}

void *ListLoad(void *ParentWin, char *FileToLoad, int ShowFlags)
{
    (void)ShowFlags;

    const char *label = NULL;
    GdkPixbuf *pixbuf = load_raw(FileToLoad, &label);
    if (!pixbuf)
        return NULL;

    ViewState *state = g_new0(ViewState, 1);
    view_state_set(state, pixbuf, FileToLoad, label);

    GtkWidget *area = gtk_drawing_area_new();
    gtk_widget_add_events(area, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(area, "button-press-event", G_CALLBACK(on_button_press), NULL);
    g_object_set_data_full(G_OBJECT(area), DATA_KEY, state, view_state_free);
    g_signal_connect(area, "expose-event", G_CALLBACK(on_expose), state);

    gtk_container_add(GTK_CONTAINER((GtkWidget *)ParentWin), area);

    GtkWidget *toplevel = gtk_widget_get_toplevel(area);
    if (GTK_IS_WINDOW(toplevel))
        g_signal_connect_object(toplevel, "key-press-event",
                                G_CALLBACK(on_toplevel_key_press), area, 0);

    gtk_widget_show_all(area);
    return area;
}

int ListLoadNext(void *ParentWin, void *PluginWin, char *FileToLoad, int ShowFlags)
{
    (void)ParentWin;
    (void)ShowFlags;

    ViewState *state = g_object_get_data(G_OBJECT(PluginWin), DATA_KEY);
    if (!state)
        return LISTPLUGIN_ERROR;

    const char *label = NULL;
    GdkPixbuf *pixbuf = load_raw(FileToLoad, &label);
    if (!pixbuf)
        return LISTPLUGIN_ERROR;

    view_state_set(state, pixbuf, FileToLoad, label);
    gtk_widget_queue_draw(GTK_WIDGET(PluginWin));
    return LISTPLUGIN_OK;
}

void ListCloseWindow(void *ListWin)
{
    gtk_widget_destroy(GTK_WIDGET(ListWin));
}

void ListGetDetectString(char *DetectString, int maxlen)
{
    g_strlcpy(DetectString, DETECT_STRING, maxlen);
}
