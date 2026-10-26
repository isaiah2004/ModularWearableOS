/* Frozen original callback before stripe-level rejection. */
static void draw_entry_reference(gfx_fb *fb, int e, float y, float h, int pressed)
{
    const sentry *en = &E[e];
    float bdy, bk = build_k(y, &bdy);
    if (bk <= 0) return;
    y += bdy;
    float cy = y + h * 0.5f;
    float k = row_baking ? 1.0f : qfade(cy) * bk;
    int lx = UI_ROW_X0, rx = UI_ROW_X1;

    if (en->kind == K_HDR) {
        ui_label(fb, lx, (int)(y + h - 16), en->label, ui_dim(C_DIS, k), UI_LEFT);
        return;
    }
    mtween plain;
    if (row_baking) tw_set(&plain, 0);
    if (en->kind == K_DANGER) {
        arm_pill(fb, cy, row_baking ? &plain : &S.arm_tw[1], "RESET SETTINGS", "TAP AGAIN TO RESET", S.arm_t, pressed,
                 row_baking ? 1.0f : 1.0f - q8(mtw_value(&S.grey_tw)), k, 0);
        return;
    }
    if (en->kind == K_ARM) {                         /* Restart: the power sheet's pill and morph */
        arm_pill(fb, cy, row_baking ? &plain : &S.arm_tw[0], "RESTART", "TAP AGAIN TO RESTART", S.arm_t,
                 pressed && mtw_value(&S.arm_tw[0]) <= 0, 1, k, 1);
        return;
    }
    if (pressed) ui_rrect(fb, lx - 20, y + 5, rx - lx + 40, h - 10, (h - 10) * 0.5f, C_SURF2);
    int id = en->id;
    int lw = ui_text_width(UI_F_BODY, en->label, 0);
    const int vx = rx - (UI_ROW_X1 - UI_VALUE_X);    /* value right edge (384 in the column) */
    int has_v = en->kind == K_NAV || en->kind == K_FLIP;
    int vk = has_v && !row_baking ? m_frame_k(S.vt0[id]) : VAL_F;      /* frames into a value swap */
    int und = has_v && value_under(en->label, S.val[id], S.unit[id]);
    int und_o = has_v && vk < VAL_F && value_under(en->label, S.oval[id], S.ounit[id]);
    /* label line: centred, or raised 12 px when the value sits under it (across a swap) */
    float tl = vk >= VAL_F ? (float)und : m_lerp((float)und_o, (float)und, mf_at(&MF_STEADY6, vk));
    int bl = cap_base(UI_F_BODY, cy) - (int)floorf(tl * 12.0f + 0.5f);
    /* Fixed control and label columns, independent of the row's screen position. */
    int cl = rx;
    switch (en->kind) {
    case K_TOG: cl = rx - 76; break;
    case K_NAV: case K_FLIP: cl = (S.val[id][0] && !und) ? vx - value_w(S.val[id], S.unit[id]) : vx; break;
    default: break;
    }
    int tx = lx;
    ui_text(fb, UI_F_BODY, tx, bl, en->label, ui_dim(C_PRI, k), UI_LEFT, 0);
    if (row_baking && entry_split(e)) return;
    switch (en->kind) {
    case K_TOG: {
        int gap = cl - (tx + lw);
        float vk2 = k * (gap >= ROW_GAP ? 1.0f : q8((gap - 8) / (float)(ROW_GAP - 8)));
        if (vk2 > 0) toggle_k(fb, rx - 38, cy, (row_baking ? S.tog[id].to : mtw_value(&S.tog[id])), vk2, tog_absent(id));
        break;
    }
    case K_NAV: case K_FLIP:
        if (en->kind == K_NAV) chevron(fb, rx, cy, ui_dim(C_DIS, k));
        else flipglyph(fb, rx, cy, ui_dim(C_DIS, k));
        if (vk >= VAL_F) row_value(fb, S.val[id], S.unit[id], und, vx, tx, lw, cy, k, 1, 0);
        else {
            float ko, ki; int dyo, dyi;
            val_swap(vk, &ko, &dyo, &ki, &dyi);
            if (ko > 0) row_value(fb, S.oval[id], S.ounit[id], und_o, vx, tx, lw, cy, k, ko, dyo);
            if (ki > 0) row_value(fb, S.val[id], S.unit[id], und, vx, tx, lw, cy, k, ki, dyi);
        }
        break;
    default: break;
    }
}
