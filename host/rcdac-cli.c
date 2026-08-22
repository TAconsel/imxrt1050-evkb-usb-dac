/* Minimal CLI for the DAC, mostly to prove the transport. SPDX: BSD-3-Clause */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rcdac.h"

static void show(const rc_usb_status_t *s)
{
    printf("filter    : %s\n", s->filter);
    printf("taps      : %u @ %u Hz source\n", s->taps, s->srcRate);
    printf("correction: %s\n", s->bypass ? "BYPASSED" : "engaged");
    printf("preamp    : %+.1f dB\n", (double)s->preamp);
    printf("cpu       : %u%%   blocks %u   underruns %u   clips %u\n",
           s->cpuPercent, s->blocks, s->underruns, s->clips);
    printf("last upload: %s\n", rc_ir_result_text(s->irResult));
    printf("eq        :");
    for (int b = 0; b < s->eqBands; b++)
    {
        printf(" %+.1f", (double)s->eq[b]);
    }
    printf("\n");
}

static void prog(uint32_t done, uint32_t total, void *u)
{
    (void)u;
    printf("\ruploading %u/%u bytes (%u%%)", done, total, done * 100 / total);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    const char *err = NULL;
    rc_dev *d = rc_open(&err);
    if (d == NULL)
    {
        fprintf(stderr, "error: %s\n", err);
        return 1;
    }
    int rc = 0;

    if (argc < 2 || strcmp(argv[1], "status") == 0)
    {
        rc_usb_status_t s;
        if (rc_status(d, &s, &err)) { show(&s); } else { fprintf(stderr, "error: %s\n", err); rc = 1; }
    }
    else if (strcmp(argv[1], "bypass") == 0 && argc == 3)
    {
        if (!rc_set_bypass(d, atoi(argv[2]) != 0, &err)) { fprintf(stderr, "error: %s\n", err); rc = 1; }
    }
    else if (strcmp(argv[1], "preamp") == 0 && argc == 3)
    {
        if (!rc_set_preamp(d, (float)atof(argv[2]), &err)) { fprintf(stderr, "error: %s\n", err); rc = 1; }
    }
    else if (strcmp(argv[1], "eq") == 0 && argc == 4)
    {
        if (!rc_set_eq(d, atoi(argv[2]), (float)atof(argv[3]), &err)) { fprintf(stderr, "error: %s\n", err); rc = 1; }
    }
    else if (strcmp(argv[1], "ir") == 0 && argc == 3)
    {
        FILE *f = fopen(argv[2], "rb");
        if (f == NULL) { perror(argv[2]); rc_close(d); return 1; }
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        uint8_t *buf = malloc((size_t)n);
        if (fread(buf, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "short read\n"); rc = 1; }
        fclose(f);
        if (rc == 0 && !rc_upload_ir(d, buf, (uint32_t)n, prog, NULL, &err))
        {
            fprintf(stderr, "\nerror: %s\n", err); rc = 1;
        }
        else if (rc == 0)
        {
            rc_usb_status_t s;
            printf("\n");
            if (rc_status(d, &s, &err)) { show(&s); }
        }
        free(buf);
    }
    else
    {
        fprintf(stderr, "usage: %s [status | bypass 0|1 | preamp <dB> | eq <band> <dB> | ir <file.wav>]\n", argv[0]);
        rc = 2;
    }
    rc_close(d);
    return rc;
}
