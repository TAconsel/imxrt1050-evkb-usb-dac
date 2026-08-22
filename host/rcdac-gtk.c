/*
 * GTK4 control panel for the RT1050 room-correction DAC.
 *
 * Talks to the board over vendor control transfers on the audio device's endpoint 0
 * (see rcdac.h), so no extra cable and playback is never interrupted.
 *
 * Laid out like a mixer: preamp fader, sixteen EQ faders, then the four meters.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <gtk/gtk.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "rcdac.h"

/* matches the firmware's ISO centres in roomcorr_eq.c */
static const char *const kBandLabel[16] = {
    "20",   "31",   "50",   "80",   "125",  "200",  "315",   "500",
    "800",  "1.25k","2k",   "3.15k","5k",   "8k",   "12.5k", "20k",
};
static const char *const kMeterName[4] = {"in L", "in R", "out L", "out R"};

#define RC_POLL_MS       (40)      /* 25 Hz; a status read is about 1 ms */
#define RC_STATS_EVERY   (10)      /* refresh the text block every 10th tick */

#define RC_FLOOR_DB      (-60.0)
#define RC_FAST_DECAY_DB (48.0)    /* dB/s the bar falls */
#define RC_HOLD_DECAY_DB (11.0)    /* dB/s the peak marker falls, once it starts */
#define RC_HOLD_TIME_US  (1200000) /* how long the marker sits before decaying */
#define RC_TOUCH_GRACE_US (700000)

typedef struct
{
    GtkWidget *area;
    GtkWidget *val;
    double     fastDb;   /* the bar */
    double     holdDb;   /* the slow peak marker */
    gint64     holdUntil;
} Meter;

typedef struct
{
    rc_dev    *dev;
    GtkWidget *window;
    GtkWidget *bypassBtn;
    GtkWidget *preamp;
    GtkWidget *preampVal;
    GtkWidget *eq[16];
    GtkWidget *eqVal[16];
    Meter      meter[4];
    GtkWidget *stats;
    GtkWidget *irLabel;
    GtkWidget *progress;
    GtkWidget *banner;
    gboolean   syncing;
    gint64     touched[17];  /* 0..15 EQ, 16 preamp */
    int        tick;
} App;

static gboolean recently_touched(App *a, int idx)
{
    return (g_get_monotonic_time() - a->touched[idx]) < RC_TOUCH_GRACE_US;
}

/* ----------------------------------------------------------------- meter ---- */

static double db_to_frac(double db)
{
    double f = (db - RC_FLOOR_DB) / (0.0 - RC_FLOOR_DB);
    return (f < 0.0) ? 0.0 : ((f > 1.0) ? 1.0 : f);
}

/*
 * Vertical bar drawn bottom-up, coloured in three zones so a hot signal is obvious
 * without reading the number, plus a thin marker showing the slowly decaying peak.
 */
static void meter_draw(GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer user)
{
    const Meter *m = user;
    (void)area;

    cairo_set_source_rgb(cr, 0.11, 0.12, 0.14);
    cairo_rectangle(cr, 0, 0, w, h);
    cairo_fill(cr);

    const double zones[3]  = {-6.0, -1.0, 0.0};
    const double rgb[3][3] = {{0.18, 0.76, 0.36}, {0.90, 0.72, 0.20}, {0.88, 0.25, 0.25}};
    double from = RC_FLOOR_DB;

    for (int z = 0; z < 3; z++)
    {
        double to = (m->fastDb < zones[z]) ? m->fastDb : zones[z];
        if (to > from)
        {
            double y0 = h * (1.0 - db_to_frac(to));
            double y1 = h * (1.0 - db_to_frac(from));
            cairo_set_source_rgb(cr, rgb[z][0], rgb[z][1], rgb[z][2]);
            cairo_rectangle(cr, 1, y0, w - 2, y1 - y0);
            cairo_fill(cr);
        }
        from = zones[z];
    }

    if (m->holdDb > RC_FLOOR_DB)
    {
        double y = h * (1.0 - db_to_frac(m->holdDb));
        cairo_set_source_rgb(cr, 0.95, 0.95, 0.97);
        cairo_rectangle(cr, 0, (y < 1.0) ? 0.0 : (y - 1.0), w, 2);
        cairo_fill(cr);
    }

    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.25); /* 0 dBFS tick */
    cairo_rectangle(cr, 0, 0, w, 1);
    cairo_fill(cr);
}

static void meter_update(Meter *m, double peakLin, double dtSec)
{
    const double db  = (peakLin > 1e-6) ? (20.0 * log10(peakLin)) : RC_FLOOR_DB;
    const gint64 now = g_get_monotonic_time();
    char buf[24];

    if (db > m->fastDb)
    {
        m->fastDb = db; /* instant attack */
    }
    else
    {
        m->fastDb -= RC_FAST_DECAY_DB * dtSec;
        if (m->fastDb < db) { m->fastDb = db; }
    }
    if (m->fastDb < RC_FLOOR_DB) { m->fastDb = RC_FLOOR_DB; }

    if (db >= m->holdDb)
    {
        m->holdDb    = db;
        m->holdUntil = now + RC_HOLD_TIME_US;
    }
    else if (now > m->holdUntil)
    {
        m->holdDb -= RC_HOLD_DECAY_DB * dtSec;
        if (m->holdDb < m->fastDb) { m->holdDb = m->fastDb; }
    }
    if (m->holdDb < RC_FLOOR_DB) { m->holdDb = RC_FLOOR_DB; }

    /* the number tracks the marker, which is the figure worth reading off */
    if (m->holdDb <= RC_FLOOR_DB) { snprintf(buf, sizeof(buf), "-inf"); }
    else                          { snprintf(buf, sizeof(buf), "%.1f", m->holdDb); }
    gtk_label_set_text(GTK_LABEL(m->val), buf);
    gtk_widget_queue_draw(m->area);
}

/* ---------------------------------------------------------------- widgets ---- */

static void show_value(GtkRange *r)
{
    GtkWidget *lbl = g_object_get_data(G_OBJECT(r), "valuelabel");
    char buf[32];

    if (lbl != NULL)
    {
        snprintf(buf, sizeof(buf), "%+.1f", gtk_range_get_value(r));
        gtk_label_set_text(GTK_LABEL(lbl), buf);
    }
}

static void on_double_click(GtkGestureClick *g, gint n_press, gdouble x, gdouble y, gpointer user)
{
    (void)x;
    (void)y;
    if (n_press == 2)
    {
        gtk_range_set_value(GTK_RANGE(user), 0.0);
        gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
    }
}

static void add_double_click_reset(GtkWidget *scale)
{
    GtkGesture *g = gtk_gesture_click_new();

    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(g), GDK_BUTTON_PRIMARY);
    /*
     * Capture phase on purpose: GtkScale's own click and drag gestures claim the
     * sequence in the bubble phase, so the second press would never reach us.
     */
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(g), GTK_PHASE_CAPTURE);
    g_signal_connect(g, "pressed", G_CALLBACK(on_double_click), scale);
    gtk_widget_add_controller(scale, GTK_EVENT_CONTROLLER(g));
}

static void set_banner(App *a, const char *msg, gboolean bad)
{
    gtk_label_set_text(GTK_LABEL(a->banner), msg);
    gtk_widget_remove_css_class(a->banner, "error");
    gtk_widget_remove_css_class(a->banner, "dim-label");
    gtk_widget_add_css_class(a->banner, bad ? "error" : "dim-label");
}

/*
 * One fader as a strip: value on top, vertical scale, name below. GTK vertical ranges
 * run low-at-top by default, so they need inverting to read like a fader.
 */
static GtkWidget *fader_column(GtkWidget **scaleOut, GtkWidget **valOut, const char *name,
                               double lo, double hi, int width, gboolean bold)
{
    GtkWidget *col   = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *val   = gtk_label_new("+0.0");
    GtkWidget *lbl   = gtk_label_new(name);
    GtkWidget *scale = gtk_scale_new_with_range(GTK_ORIENTATION_VERTICAL, lo, hi, 0.5);

    gtk_range_set_inverted(GTK_RANGE(scale), TRUE);
    gtk_scale_set_draw_value(GTK_SCALE(scale), FALSE);
    gtk_scale_add_mark(GTK_SCALE(scale), 0.0, GTK_POS_LEFT, NULL);
    gtk_widget_set_vexpand(scale, TRUE);
    gtk_widget_set_size_request(scale, width, 200);
    gtk_widget_set_tooltip_text(scale, "drag to adjust, double-click to reset to 0 dB");
    g_object_set_data(G_OBJECT(scale), "valuelabel", val);
    add_double_click_reset(scale);

    gtk_widget_add_css_class(val, "monospace");
    gtk_widget_add_css_class(lbl, bold ? "heading" : "dim-label");
    gtk_box_append(GTK_BOX(col), val);
    gtk_box_append(GTK_BOX(col), scale);
    gtk_box_append(GTK_BOX(col), lbl);

    *scaleOut = scale;
    *valOut   = val;
    return col;
}

static GtkWidget *meter_column(App *a, int i)
{
    GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    Meter     *m   = &a->meter[i];

    m->val    = gtk_label_new("-inf");
    m->area   = gtk_drawing_area_new();
    m->fastDb = RC_FLOOR_DB;
    m->holdDb = RC_FLOOR_DB;

    gtk_widget_set_size_request(m->area, 20, 200);
    gtk_widget_set_vexpand(m->area, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(m->area), meter_draw, m, NULL);
    gtk_widget_set_tooltip_text(m->area,
                                "peak dBFS, -60 at the bottom.\n"
                                "bar falls fast, the line is a slow-decay peak hold");

    GtkWidget *lbl = gtk_label_new(kMeterName[i]);
    gtk_widget_add_css_class(m->val, "monospace");
    gtk_widget_add_css_class(lbl, "dim-label");
    gtk_box_append(GTK_BOX(col), m->val);
    gtk_box_append(GTK_BOX(col), m->area);
    gtk_box_append(GTK_BOX(col), lbl);
    return col;
}

/* ------------------------------------------------------------------ poll ---- */

static gboolean refresh(gpointer user)
{
    App *a = user;
    rc_usb_status_t s;
    const char *err = NULL;
    char buf[512];

    if (a->dev == NULL)
    {
        a->dev = rc_open(&err);
        if (a->dev == NULL)
        {
            set_banner(a, err, TRUE);
            return G_SOURCE_CONTINUE;
        }
        set_banner(a, "connected", FALSE);
    }

    if (!rc_status(a->dev, &s, &err))
    {
        set_banner(a, err, TRUE);
        rc_close(a->dev);
        a->dev = NULL; /* re-open next tick, so unplugging is not fatal */
        return G_SOURCE_CONTINUE;
    }

    for (int i = 0; i < 4; i++)
    {
        meter_update(&a->meter[i], (double)s.peak[i], RC_POLL_MS / 1000.0);
    }

    a->syncing = TRUE;

    gtk_button_set_label(GTK_BUTTON(a->bypassBtn),
                         s.bypass ? "Correction BYPASSED" : "Correction ENGAGED");
    gtk_widget_remove_css_class(a->bypassBtn, s.bypass ? "suggested-action" : "destructive-action");
    gtk_widget_add_css_class(a->bypassBtn, s.bypass ? "destructive-action" : "suggested-action");

    if (!recently_touched(a, 16))
    {
        gtk_range_set_value(GTK_RANGE(a->preamp), s.preamp);
        snprintf(buf, sizeof(buf), "%+.1f", (double)s.preamp);
        gtk_label_set_text(GTK_LABEL(a->preampVal), buf);
    }
    for (int b = 0; b < 16 && b < s.eqBands; b++)
    {
        if (!recently_touched(a, b))
        {
            gtk_range_set_value(GTK_RANGE(a->eq[b]), s.eq[b]);
            snprintf(buf, sizeof(buf), "%+.1f", (double)s.eq[b]);
            gtk_label_set_text(GTK_LABEL(a->eqVal[b]), buf);
        }
    }

    /* the text block does not need 25 Hz, and relaying it out that often is wasteful */
    if ((a->tick++ % RC_STATS_EVERY) == 0)
    {
        snprintf(buf, sizeof(buf),
                 "filter      %s\n"
                 "taps        %u @ %u Hz source\n"
                 "dsp load    %u %% now, peak %u %% of the block budget\n"
                 "blocks      %u\n"
                 "underruns   %u        clips %u",
                 s.filter, s.taps, s.srcRate, s.cpuPercent, s.cpuPeakPercent, s.blocks,
                 s.underruns, s.clips);
        gtk_label_set_text(GTK_LABEL(a->stats), buf);
    }

    a->syncing = FALSE;
    return G_SOURCE_CONTINUE;
}

/* --------------------------------------------------------------- handlers ---- */

static void on_bypass(GtkButton *btn, gpointer user)
{
    App *a = user;
    const char *err = NULL;
    gboolean nowBypassed = (strstr(gtk_button_get_label(btn), "BYPASSED") != NULL);

    if (a->dev != NULL && !rc_set_bypass(a->dev, !nowBypassed, &err))
    {
        set_banner(a, err, TRUE);
    }
    refresh(a);
}

static void on_preamp(GtkRange *r, gpointer user)
{
    App *a = user;
    const char *err = NULL;

    show_value(r);
    if (!a->syncing)
    {
        a->touched[16] = g_get_monotonic_time();
        if (a->dev != NULL && !rc_set_preamp(a->dev, (float)gtk_range_get_value(r), &err))
        {
            set_banner(a, err, TRUE);
        }
    }
}

static void on_eq(GtkRange *r, gpointer user)
{
    App *a = user;
    const char *err = NULL;
    int band = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(r), "band"));

    show_value(r);
    if (!a->syncing)
    {
        a->touched[band] = g_get_monotonic_time();
        if (a->dev != NULL && !rc_set_eq(a->dev, band, (float)gtk_range_get_value(r), &err))
        {
            set_banner(a, err, TRUE);
        }
    }
}

static void on_flatten(GtkButton *btn, gpointer user)
{
    App *a = user;
    const char *err = NULL;
    (void)btn;

    for (int b = 0; b < 16; b++)
    {
        gtk_range_set_value(GTK_RANGE(a->eq[b]), 0.0);
        if (a->dev != NULL && !rc_set_eq(a->dev, b, 0.0f, &err))
        {
            set_banner(a, err, TRUE);
            break;
        }
    }
}

static void on_reset_stats(GtkButton *btn, gpointer user)
{
    App *a = user;
    const char *err = NULL;
    (void)btn;

    if (a->dev != NULL && !rc_reset_stats(a->dev, &err))
    {
        set_banner(a, err, TRUE);
    }
    for (int i = 0; i < 4; i++)
    {
        a->meter[i].holdDb = RC_FLOOR_DB; /* drop the peak markers too */
    }
    refresh(a);
}

static void upload_progress(uint32_t done, uint32_t total, void *user)
{
    App *a = user;
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(a->progress), (double)done / (double)total);
    while (g_main_context_pending(NULL))
    {
        g_main_context_iteration(NULL, FALSE);
    }
}

static void on_ir_chosen(GObject *src, GAsyncResult *res, gpointer user)
{
    App *a = user;
    const char *err = NULL;
    GError *gerr = NULL;
    GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, &gerr);

    if (file == NULL)
    {
        g_clear_error(&gerr);
        return; /* cancelled */
    }

    gchar *data = NULL;
    gsize  len  = 0;
    if (!g_file_load_contents(file, NULL, &data, &len, NULL, &gerr))
    {
        set_banner(a, gerr->message, TRUE);
        g_clear_error(&gerr);
        g_object_unref(file);
        return;
    }

    gtk_label_set_text(GTK_LABEL(a->irLabel), "uploading, audio will glitch briefly...");
    if (a->dev == NULL || !rc_upload_ir(a->dev, (const uint8_t *)data, (uint32_t)len,
                                        upload_progress, a, &err))
    {
        set_banner(a, err ? err : "not connected", TRUE);
        gtk_label_set_text(GTK_LABEL(a->irLabel), err ? err : "upload failed");
    }
    else
    {
        rc_usb_status_t s;
        if (rc_status(a->dev, &s, &err))
        {
            char msg[160];
            snprintf(msg, sizeof(msg), "loaded: %s", s.filter);
            gtk_label_set_text(GTK_LABEL(a->irLabel), msg);
        }
    }
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(a->progress), 0.0);
    g_free(data);
    g_object_unref(file);
}

static void on_ir_click(GtkButton *btn, gpointer user)
{
    App *a = user;
    (void)btn;
    GtkFileDialog *dlg = gtk_file_dialog_new();
    GtkFileFilter *f   = gtk_file_filter_new();

    gtk_file_filter_set_name(f, "WAV impulse response");
    gtk_file_filter_add_pattern(f, "*.wav");
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(filters, f);
    gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(filters));
    gtk_file_dialog_set_title(dlg, "Choose an impulse response (48 or 96 kHz)");
    gtk_file_dialog_open(dlg, GTK_WINDOW(a->window), NULL, on_ir_chosen, a);
    g_object_unref(filters);
    g_object_unref(f);
    g_object_unref(dlg);
}

/* -------------------------------------------------------------------- ui ---- */

static GtkWidget *heading(const char *text)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
    gtk_widget_add_css_class(l, "heading");
    return l;
}

static void activate(GtkApplication *app, gpointer user)
{
    App *a = user;

    a->window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(a->window), "RT1050 Room Correction");
    gtk_window_set_default_size(GTK_WINDOW(a->window), 1020, 780);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(box, 14);
    gtk_widget_set_margin_bottom(box, 14);
    gtk_widget_set_margin_start(box, 16);
    gtk_widget_set_margin_end(box, 16);

    a->banner = gtk_label_new("connecting...");
    gtk_label_set_xalign(GTK_LABEL(a->banner), 0.0f);
    gtk_widget_add_css_class(a->banner, "dim-label");
    gtk_box_append(GTK_BOX(box), a->banner);

    a->bypassBtn = gtk_button_new_with_label("Correction ENGAGED");
    g_signal_connect(a->bypassBtn, "clicked", G_CALLBACK(on_bypass), a);
    gtk_box_append(GTK_BOX(box), a->bypassBtn);

    /* ---- mixer strip: preamp | EQ | meters ---- */
    GtkWidget *strip = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

    GtkWidget *pre = fader_column(&a->preamp, &a->preampVal, "preamp", -40.0, 12.0, 38, TRUE);
    g_signal_connect(a->preamp, "value-changed", G_CALLBACK(on_preamp), a);
    gtk_box_append(GTK_BOX(strip), pre);
    gtk_box_append(GTK_BOX(strip), gtk_separator_new(GTK_ORIENTATION_VERTICAL));

    GtkWidget *eqBox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_set_homogeneous(GTK_BOX(eqBox), TRUE);
    gtk_widget_set_hexpand(eqBox, TRUE);
    for (int b = 0; b < 16; b++)
    {
        GtkWidget *col =
            fader_column(&a->eq[b], &a->eqVal[b], kBandLabel[b], -12.0, 12.0, 30, FALSE);
        g_object_set_data(G_OBJECT(a->eq[b]), "band", GINT_TO_POINTER(b));
        g_signal_connect(a->eq[b], "value-changed", G_CALLBACK(on_eq), a);
        gtk_box_append(GTK_BOX(eqBox), col);
    }
    gtk_box_append(GTK_BOX(strip), eqBox);

    gtk_box_append(GTK_BOX(strip), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
    GtkWidget *meters = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    for (int i = 0; i < 4; i++)
    {
        gtk_box_append(GTK_BOX(meters), meter_column(a, i));
    }
    gtk_box_append(GTK_BOX(strip), meters);

    GtkWidget *scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_NEVER);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), strip);
    gtk_widget_set_vexpand(scroller, TRUE);
    gtk_box_append(GTK_BOX(box), scroller);

    GtkWidget *hint = gtk_label_new(
        "faders: double-click to reset to 0 dB     |     meters: peak dBFS, bar falls fast, "
        "line is a slow peak hold");
    gtk_label_set_xalign(GTK_LABEL(hint), 0.0f);
    gtk_widget_add_css_class(hint, "dim-label");
    gtk_box_append(GTK_BOX(box), hint);

    GtkWidget *btnRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *flat   = gtk_button_new_with_label("Flatten EQ");
    GtkWidget *rst    = gtk_button_new_with_label("Reset statistics");
    GtkWidget *irBtn  = gtk_button_new_with_label("Upload IR .wav ...");
    g_signal_connect(flat, "clicked", G_CALLBACK(on_flatten), a);
    g_signal_connect(rst, "clicked", G_CALLBACK(on_reset_stats), a);
    g_signal_connect(irBtn, "clicked", G_CALLBACK(on_ir_click), a);
    gtk_widget_set_hexpand(flat, TRUE);
    gtk_widget_set_hexpand(rst, TRUE);
    gtk_widget_set_hexpand(irBtn, TRUE);
    gtk_box_append(GTK_BOX(btnRow), flat);
    gtk_box_append(GTK_BOX(btnRow), rst);
    gtk_box_append(GTK_BOX(btnRow), irBtn);
    gtk_box_append(GTK_BOX(box), btnRow);

    a->progress = gtk_progress_bar_new();
    gtk_box_append(GTK_BOX(box), a->progress);
    a->irLabel = gtk_label_new("48 or 96 kHz, mono or stereo, PCM 16/24/32 or float32");
    gtk_label_set_xalign(GTK_LABEL(a->irLabel), 0.0f);
    gtk_widget_add_css_class(a->irLabel, "dim-label");
    gtk_box_append(GTK_BOX(box), a->irLabel);

    gtk_box_append(GTK_BOX(box), heading("Status"));
    a->stats = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(a->stats), 0.0f);
    gtk_label_set_selectable(GTK_LABEL(a->stats), TRUE);
    gtk_widget_add_css_class(a->stats, "monospace");
    gtk_box_append(GTK_BOX(box), a->stats);

    gtk_window_set_child(GTK_WINDOW(a->window), box);
    gtk_window_present(GTK_WINDOW(a->window));

    refresh(a);
    g_timeout_add(RC_POLL_MS, refresh, a);
}

int main(int argc, char **argv)
{
    App a = {0};
    const char *err = NULL;

    a.dev = rc_open(&err); /* absence is not fatal: refresh() keeps retrying */

    GtkApplication *app = gtk_application_new("net.consel.rt1050dac", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), &a);
    int rc = g_application_run(G_APPLICATION(app), argc, argv);

    g_object_unref(app);
    rc_close(a.dev);
    return rc;
}
