/* TACK 6.2: LOOP phrase export pull (cmd 82). Reverse of inbox ingest. */
enum { ED_PHRASE_EXPORT = 82 };
extern int tack_ed_phrase_export(uint8_t *out, uint32_t cap);

static int ed_phrase_handle(uint32_t cmd, const uint8_t *a, uint32_t n)
{
    uint8_t buf[64];
    int got;
    (void)a; (void)n;
    if (cmd != ED_PHRASE_EXPORT)
        return 0;
    got = tack_ed_phrase_export(buf, (uint32_t)sizeof buf);
    if (got <= 0) {
        ed_b(0); /* nch = 0 → not armed / empty */
        return 1;
    }
    {
        uint32_t i;
        for (i = 0; i < (uint32_t)got; i++)
            ed_b(buf[i]);
    }
    return 1;
}
