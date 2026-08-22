/*
 * GTK4 control panel for the RT1050 room-correction DAC.
 *
 * Talks to the board over vendor control transfers on the audio device's endpoint 0
 * (see rcdac.h), so no extra cable and playback is never interrupted.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <gtk/gtk.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
#include "rcdac.h"

/* matches the firmware's ISO centres in roomcorr_eq.c */
static const char *const kBandLabel[16] = {
    "20",   "31",   "50",   "80",   "125",  "200",  "315",   "500",
    "800",  "1.25k","2k",   "3.15k","5k",   "8k",   "12.5k", "20k",
};

typedef struct
{
    rc_dev      *dev;
    GtkWidget   *window;
    GtkWidget   *bypassBtn;
    GtkWidget   *preamp;
    GtkWidget   *preampVal;
    GtkWidget   *eq[16];
    GtkWidget   *eqVal[16];
    GtkWidget   *stats;
    GtkWidget   *irLabel;
    GtkWidget   *progress;
    GtkWidget   *banner;
    GtkWidget   *meter[4];      /* in L, in R, out L, out R */
    GtkWidget   *meterVal[4];
    double       meterDb[4];    /* displayed level, with decay applied */
    gboolean     syncing;   /* suppress change handlers while pushing device state in */
    /*
     * When a slider was last moved by the user, index 16 being the preamp. At a 200 ms
     * poll, relying on keyboard focus alone to decide "don't overwrite this one" is too
     * fragile -- a touchpad drag may not focus the widget -- so a short grace period
     * after any user movement keeps the poll from snapping the slider back mid-gesture.
     */
    gint64       touched[17];
} App;

#define RC_TOUCH_GRACE_US (700000) /* 0.7 s */

static gboolean recently_touched(App *a, int idx)
{
    return (g_get_monotonic_time() - a->touched[idx]) < RC_TOUCH_GRACE_US;
}

#define RC_METER_FLOOR_DB (-60.0)
#define RC_METER_DECAY_DB (36.0) /* dB per second the bar falls once the peak passes */

static const char *const kMeterName[4] = {"in L", "in R", "out L", "out R"};

static double lin_to_db(double v)
{
    return (v > 1e-6) ? (20.0 * log10(v)) : RC_METER_FLOOR_DB;
}

/*
 * Peak-hold on the device, decay here. Instant attack so a transient is never smoothed
 * away, then a steady fall, which is what makes a meter readable rather than a flicker.
 */
static void meter_update(App *a, int i, double peakLin, double dtSec)
{
    double db = lin_to_db(peakLin);
    char   buf[24];

    if (db > a->meterDb[i])
    {
        a->meterDb[i] = db; /* attack */
    }
    else
    {
        a->meterDb[i] -= RC_METER_DECAY_DB * dtSec;
        if (a->meterDb[i] < db)
        {
            a->meterDb[i] = db;
        }
    }
    if (a->meterDb[i] < RC_METER_FLOOR_DB)
    {
        a->meterDb[i] = RC_METER_FLOOR_DB;
    }

    gtk_level_bar_set_value(GTK_LEVEL_BAR(a->meter[i]),
                            (a->meterDb[i] - RC_METER_FLOOR_DB) / (0.0 - RC_METER_FLOOR_DB));
    if (a->meterDb[i] <= RC_METER_FLOOR_DB)
    {
        snprintf(buf, sizeof(buf), "  -inf");
    }
    else
    {
        snprintf(buf, sizeof(buf), "%+6.1f", a->meterDb[i]);
    }
    gtk_label_set_text(GTK_LABEL(a->meterVal[i]), buf);
}

/* Label the slider owns, so a handler can refresh it without waiting for the poll. */
static void show_value(GtkRange *r, const char *suffix)
{
    GtkWidget *lbl = g_object_get_data(G_OBJECT(r), "valuelabel");
    char buf[32];

    if (lbl != NULL)
    {
        snprintf(buf, sizeof(buf), "%+.1f%s", gtk_range_get_value(r), suffix);
        gtk_label_set_text(GTK_LABEL(lbl), buf);
    }
}

static void on_double_click(GtkGestureClick *g, gint n_press, gdouble x, gdouble y,
                            gpointer user)
{
    (void)x;
    (void)y;
    if (n_press == 2)
    {
        gtk_range_set_value(GTK_RANGE(user), 0.0);
        gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
    }
}

/*! Double-clicking anywhere on a slider snaps it back to 0 dB. */
static void add_double_click_reset(GtkWidget *scale)
{
    GtkGesture *g = gtk_gesture_click_new();

    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(g), GDK_BUTTON_PRIMARY);
    /*
     * Capture phase on purpose. GtkScale has its own click and drag gestures; in the
     * bubble phase they claim the sequence first and the second press never reaches us.
     * Capturing lets single clicks fall through untouched and only intercepts the pair.
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
        a->dev = NULL; /* re-open on the next tick, so unplugging is not fatal */
        return G_SOURCE_CONTINUE;
    }

    a->syncing = TRUE;

    gtk_button_set_label(GTK_BUTTON(a->bypassBtn),
                         s.bypass ? "Correction BYPASSED" : "Correction ENGAGED");
    gtk_widget_remove_css_class(a->bypassBtn, s.bypass ? "suggested-action" : "destructive-action");
    gtk_widget_add_css_class(a->bypassBtn, s.bypass ? "destructive-action" : "suggested-action");

    if (!recently_touched(a, 16))
    {
        gtk_range_set_value(GTK_RANGE(a->preamp), s.preamp);
        snprintf(buf, sizeof(buf), "%+.1f dB", (double)s.preamp);
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

    for (int i = 0; i < 4; i++)
    {
        meter_update(a, i, (double)s.peak[i], 0.1); /* the poll period, see g_timeout_add */
    }

    snprintf(buf, sizeof(buf),
             "filter      %s\n"
             "taps        %u @ %u Hz source\n"
             "dsp load    %u %% now, peak %u %% of the block budget\n"
             "blocks      %u\n"
             "underruns   %u        clips %u",
             s.filter, s.taps, s.srcRate, s.cpuPercent, s.cpuPeakPercent, s.blocks,
             s.underruns, s.clips);
    gtk_label_set_text(GTK_LABEL(a->stats), buf);

    a->syncing = FALSE;
    return G_SOURCE_CONTINUE;
}

/* --------------------------------------------------------------- handlers ---- */

static void on_bypass(GtkButton *btn, gpointer user)
{
    App *a = user;
    const char *err = NULL;
    const char *lbl = gtk_button_get_label(btn);
    gboolean nowBypassed = (strstr(lbl, "BYPASSED") != NULL);

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

    show_value(r, " dB"); /* immediately, not on the next poll */
    if (!a->syncing)
    {
        a->touched[16] = g_get_monotonic_time();
    }
    if (!a->syncing && a->dev != NULL && !rc_set_preamp(a->dev, (float)gtk_range_get_value(r), &err))
    {
        set_banner(a, err, TRUE);
    }
}

static void on_eq(GtkRange *r, gpointer user)
{
    App *a = user;
    const char *err = NULL;
    int band = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(r), "band"));

    show_value(r, "");
    if (!a->syncing)
    {
        a->touched[band] = g_get_monotonic_time();
    }
    if (!a->syncing && a->dev != NULL && !rc_set_eq(a->dev, band, (float)gtk_range_get_value(r), &err))
    {
        set_banner(a, err, TRUE);
    }
}

static void on_flatten(GtkButton *btn, gpointer user)
{
    App *a = user;
    const char *err = NULL;
    (void)btn;

    for (int b = 0; b < 16; b++)
    {
        gtk_range_set_value(GTK_RANGE(a->eq[b]), 0.0); /* sends via value-changed */
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
    refresh(a);
}

static void upload_progress(uint32_t done, uint32_t total, void *user)
{
    App *a = user;
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(a->progress), (double)done / (double)total);
    /* the whole upload is ~0.2 s, but keep the bar honest anyway */
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
        gtk_label_set_text(GTK_LABEL(a->irLabel), "upload failed");
    }
    else
    {
        rc_usb_status_t s;
        if (rc_status(a->dev, &s, &err))
        {
            char msg[160];
            snprintf(msg, sizeof(msg), "%s: %s", rc_ir_result_text(s.irResult), s.filter);
            gtk_label_set_text(GTK_LABEL(a->irLabel), msg);
        }
    }
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(a->progress), 0.0);
    g_free(data);
    g_object_unref(file);
    refresh(a);
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

/*
 * One EQ band as a mixer strip: value on top, vertical fader, centre frequency below.
 * GTK vertical ranges run low-at-top by default, so they need inverting to read like a
 * graphic EQ.
 */
static GtkWidget *eq_column(App *a, int band)
{
    GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *val = gtk_label_new("+0.0");
    GtkWidget *hz  = gtk_label_new(kBandLabel[band]);

    a->eq[band]    = gtk_scale_new_with_range(GTK_ORIENTATION_VERTICAL, -12.0, 12.0, 0.5);
    a->eqVal[band] = val;

    gtk_range_set_inverted(GTK_RANGE(a->eq[band]), TRUE);
    gtk_scale_set_draw_value(GTK_SCALE(a->eq[band]), FALSE);
    gtk_scale_add_mark(GTK_SCALE(a->eq[band]), 0.0, GTK_POS_LEFT, NULL);
    gtk_widget_set_vexpand(a->eq[band], TRUE);
    gtk_widget_set_size_request(a->eq[band], 34, 190);
    gtk_widget_set_tooltip_text(a->eq[band], "drag to adjust, double-click to reset to 0 dB");

    g_object_set_data(G_OBJECT(a->eq[band]), "band", GINT_TO_POINTER(band));
    g_object_set_data(G_OBJECT(a->eq[band]), "valuelabel", val);
    g_signal_connect(a->eq[band], "value-changed", G_CALLBACK(on_eq), a);
    add_double_click_reset(a->eq[band]);

    gtk_widget_add_css_class(val, "monospace");
    gtk_widget_add_css_class(hz, "dim-label");
    gtk_box_append(GTK_BOX(col), val);
    gtk_box_append(GTK_BOX(col), a->eq[band]);
    gtk_box_append(GTK_BOX(col), hz);
    return col;
}

static GtkWidget *labelled_row(const char *text, GtkWidget *mid, GtkWidget *right, int labelW)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *l   = gtk_label_new(text);

    gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
    gtk_widget_set_size_request(l, labelW, -1);
    gtk_widget_add_css_class(l, "dim-label");
    gtk_box_append(GTK_BOX(row), l);
    gtk_widget_set_hexpand(mid, TRUE);
    gtk_box_append(GTK_BOX(row), mid);
    if (right != NULL)
    {
        gtk_widget_set_size_request(right, 60, -1);
        gtk_label_set_xalign(GTK_LABEL(right), 1.0f);
        gtk_box_append(GTK_BOX(row), right);
    }
    return row;
}

static void activate(GtkApplication *app, gpointer user)
{
    App *a = user;
    GtkWidget *box, *scroller, *sect;

    a->window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(a->window), "RT1050 Room Correction");
    gtk_window_set_default_size(GTK_WINDOW(a->window), 860, 860);

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
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

    sect = gtk_label_new("Levels");
    gtk_label_set_xalign(GTK_LABEL(sect), 0.0f);
    gtk_widget_add_css_class(sect, "heading");
    gtk_box_append(GTK_BOX(box), sect);

    GtkWidget *meters = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    for (int i = 0; i < 4; i++)
    {
        a->meter[i] = gtk_level_bar_new();
        gtk_level_bar_set_mode(GTK_LEVEL_BAR(a->meter[i]), GTK_LEVEL_BAR_MODE_CONTINUOUS);
        gtk_level_bar_set_min_value(GTK_LEVEL_BAR(a->meter[i]), 0.0);
        gtk_level_bar_set_max_value(GTK_LEVEL_BAR(a->meter[i]), 1.0);
        /* -6 dBFS and -1 dBFS on a -60..0 scale, so the bar turns as it gets hot */
        gtk_level_bar_add_offset_value(GTK_LEVEL_BAR(a->meter[i]), GTK_LEVEL_BAR_OFFSET_LOW, 0.90);
        gtk_level_bar_add_offset_value(GTK_LEVEL_BAR(a->meter[i]), GTK_LEVEL_BAR_OFFSET_HIGH, 0.983);
        gtk_level_bar_add_offset_value(GTK_LEVEL_BAR(a->meter[i]), GTK_LEVEL_BAR_OFFSET_FULL, 1.0);
        gtk_widget_set_size_request(a->meter[i], -1, 14);

        a->meterVal[i] = gtk_label_new("  -inf");
        gtk_widget_add_css_class(a->meterVal[i], "monospace");
        a->meterDb[i] = RC_METER_FLOOR_DB;
        gtk_box_append(GTK_BOX(meters),
                       labelled_row(kMeterName[i], a->meter[i], a->meterVal[i], 64));
    }
    gtk_box_append(GTK_BOX(box), meters);

    GtkWidget *scaleHint = gtk_label_new("peak dBFS, -60 to 0");
    gtk_label_set_xalign(GTK_LABEL(scaleHint), 0.0f);
    gtk_widget_add_css_class(scaleHint, "dim-label");
    gtk_box_append(GTK_BOX(box), scaleHint);

    sect = gtk_label_new("Preamp");
    gtk_label_set_xalign(GTK_LABEL(sect), 0.0f);
    gtk_widget_add_css_class(sect, "heading");
    gtk_box_append(GTK_BOX(box), sect);

    a->preamp = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, -40.0, 12.0, 0.5);
    gtk_scale_set_draw_value(GTK_SCALE(a->preamp), FALSE);
    gtk_scale_add_mark(GTK_SCALE(a->preamp), 0.0, GTK_POS_BOTTOM, NULL);
    gtk_widget_set_tooltip_text(a->preamp, "drag to adjust, double-click to reset to 0 dB");
    a->preampVal = gtk_label_new("+0.0 dB");
    gtk_widget_add_css_class(a->preampVal, "monospace");
    g_object_set_data(G_OBJECT(a->preamp), "valuelabel", a->preampVal);
    g_signal_connect(a->preamp, "value-changed", G_CALLBACK(on_preamp), a);
    add_double_click_reset(a->preamp);
    gtk_box_append(GTK_BOX(box), labelled_row("gain", a->preamp, a->preampVal, 64));

    sect = gtk_label_new("16-band EQ");
    gtk_label_set_xalign(GTK_LABEL(sect), 0.0f);
    gtk_widget_add_css_class(sect, "heading");
    gtk_box_append(GTK_BOX(box), sect);

    GtkWidget *eqBox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_widget_set_halign(eqBox, GTK_ALIGN_FILL);
    gtk_box_set_homogeneous(GTK_BOX(eqBox), TRUE);
    for (int b = 0; b < 16; b++)
    {
        gtk_box_append(GTK_BOX(eqBox), eq_column(a, b));
    }
    scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_NEVER);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), eqBox);
    gtk_widget_set_vexpand(scroller, TRUE);
    gtk_box_append(GTK_BOX(box), scroller);

    GtkWidget *hint = gtk_label_new("double-click any slider to reset it to 0 dB");
    gtk_label_set_xalign(GTK_LABEL(hint), 0.0f);
    gtk_widget_add_css_class(hint, "dim-label");
    gtk_box_append(GTK_BOX(box), hint);

    GtkWidget *flat = gtk_button_new_with_label("Flatten EQ");
    g_signal_connect(flat, "clicked", G_CALLBACK(on_flatten), a);
    gtk_box_append(GTK_BOX(box), flat);

    sect = gtk_label_new("Impulse response");
    gtk_label_set_xalign(GTK_LABEL(sect), 0.0f);
    gtk_widget_add_css_class(sect, "heading");
    gtk_box_append(GTK_BOX(box), sect);

    GtkWidget *irBtn = gtk_button_new_with_label("Upload .wav ...");
    g_signal_connect(irBtn, "clicked", G_CALLBACK(on_ir_click), a);
    gtk_box_append(GTK_BOX(box), irBtn);
    a->progress = gtk_progress_bar_new();
    gtk_box_append(GTK_BOX(box), a->progress);
    a->irLabel = gtk_label_new("48 or 96 kHz, mono or stereo, PCM 16/24/32 or float32");
    gtk_label_set_xalign(GTK_LABEL(a->irLabel), 0.0f);
    gtk_widget_add_css_class(a->irLabel, "dim-label");
    gtk_box_append(GTK_BOX(box), a->irLabel);

    sect = gtk_label_new("Status");
    gtk_label_set_xalign(GTK_LABEL(sect), 0.0f);
    gtk_widget_add_css_class(sect, "heading");
    gtk_box_append(GTK_BOX(box), sect);

    GtkWidget *resetBtn = gtk_button_new_with_label("Reset statistics");
    gtk_widget_set_tooltip_text(resetBtn, "zero the block, underrun, clip and peak-load counters");
    g_signal_connect(resetBtn, "clicked", G_CALLBACK(on_reset_stats), a);
    gtk_box_append(GTK_BOX(box), resetBtn);

    a->stats = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(a->stats), 0.0f);
    gtk_label_set_selectable(GTK_LABEL(a->stats), TRUE);
    gtk_widget_add_css_class(a->stats, "monospace");
    gtk_box_append(GTK_BOX(box), a->stats);

    gtk_window_set_child(GTK_WINDOW(a->window), box);
    gtk_window_present(GTK_WINDOW(a->window));

    refresh(a);
    g_timeout_add(100, refresh, a); /* ~1 ms per read; fast enough for the meters */
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
