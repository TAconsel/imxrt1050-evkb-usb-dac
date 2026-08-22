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
    gboolean     syncing;   /* suppress change handlers while pushing device state in */
} App;

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

    /* don't fight the user while they are dragging a slider */
    GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(a->window));
    if (focus != a->preamp)
    {
        gtk_range_set_value(GTK_RANGE(a->preamp), s.preamp);
    }
    snprintf(buf, sizeof(buf), "%+.1f dB", (double)s.preamp);
    gtk_label_set_text(GTK_LABEL(a->preampVal), buf);

    for (int b = 0; b < 16 && b < s.eqBands; b++)
    {
        if (focus != a->eq[b])
        {
            gtk_range_set_value(GTK_RANGE(a->eq[b]), s.eq[b]);
        }
        snprintf(buf, sizeof(buf), "%+.1f", (double)s.eq[b]);
        gtk_label_set_text(GTK_LABEL(a->eqVal[b]), buf);
    }

    snprintf(buf, sizeof(buf),
             "filter      %s\n"
             "taps        %u @ %u Hz source\n"
             "dsp load    %u %% of the block budget\n"
             "blocks      %u\n"
             "underruns   %u        clips %u",
             s.filter, s.taps, s.srcRate, s.cpuPercent, s.blocks, s.underruns, s.clips);
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
        if (a->dev != NULL && !rc_set_eq(a->dev, b, 0.0f, &err))
        {
            set_banner(a, err, TRUE);
            break;
        }
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
    gtk_window_set_default_size(GTK_WINDOW(a->window), 620, 780);

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

    sect = gtk_label_new("Preamp");
    gtk_label_set_xalign(GTK_LABEL(sect), 0.0f);
    gtk_widget_add_css_class(sect, "heading");
    gtk_box_append(GTK_BOX(box), sect);

    a->preamp = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, -40.0, 12.0, 0.5);
    gtk_scale_set_draw_value(GTK_SCALE(a->preamp), FALSE);
    g_signal_connect(a->preamp, "value-changed", G_CALLBACK(on_preamp), a);
    a->preampVal = gtk_label_new("+0.0 dB");
    gtk_box_append(GTK_BOX(box), labelled_row("gain", a->preamp, a->preampVal, 64));

    sect = gtk_label_new("16-band EQ");
    gtk_label_set_xalign(GTK_LABEL(sect), 0.0f);
    gtk_widget_add_css_class(sect, "heading");
    gtk_box_append(GTK_BOX(box), sect);

    GtkWidget *eqBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    for (int b = 0; b < 16; b++)
    {
        char lbl[16];
        a->eq[b] = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, -12.0, 12.0, 0.5);
        gtk_scale_set_draw_value(GTK_SCALE(a->eq[b]), FALSE);
        gtk_scale_add_mark(GTK_SCALE(a->eq[b]), 0.0, GTK_POS_BOTTOM, NULL);
        g_object_set_data(G_OBJECT(a->eq[b]), "band", GINT_TO_POINTER(b));
        g_signal_connect(a->eq[b], "value-changed", G_CALLBACK(on_eq), a);
        a->eqVal[b] = gtk_label_new("+0.0");
        snprintf(lbl, sizeof(lbl), "%s Hz", kBandLabel[b]);
        gtk_box_append(GTK_BOX(eqBox), labelled_row(lbl, a->eq[b], a->eqVal[b], 64));
    }
    scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), eqBox);
    gtk_widget_set_vexpand(scroller, TRUE);
    gtk_box_append(GTK_BOX(box), scroller);

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

    a->stats = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(a->stats), 0.0f);
    gtk_label_set_selectable(GTK_LABEL(a->stats), TRUE);
    gtk_widget_add_css_class(a->stats, "monospace");
    gtk_box_append(GTK_BOX(box), a->stats);

    gtk_window_set_child(GTK_WINDOW(a->window), box);
    gtk_window_present(GTK_WINDOW(a->window));

    refresh(a);
    g_timeout_add(1000, refresh, a);
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
