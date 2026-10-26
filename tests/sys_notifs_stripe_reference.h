/* Frozen original callback before stripe-level rejection. */
static void draw_card_reference(gfx_fb *fb, const svc_notif *n, float x, float y, float h, float fade, int pressed)
{
    if (!n || fade <= 0) return;
    ui_rrect(fb, x, y, CARD_W, h, CARD_R, gdim(pressed ? C_LINE2 : C_SURF2, fade));
    int ix = (int)x + 36, hy = (int)y + 38;
    dot_icon(fb, n->icon ? n->icon : DOT_ICON_BELL, (float)ix, (float)(hy - 6), 2.3f, 1.85f, gdim(C_WHITE, fade));
    ui_label(fb, ix + 22, hy, n->app[0] ? n->app : "Phone", gdim(C_SEC, fade), UI_LEFT);
    char a[16];
    ago(a, sizeof a, n->t_epoch, N.now);
    if (a[0]) ui_label(fb, (int)(x + CARD_W) - 28, hy, a, gdim(C_DIS, fade), UI_RIGHT);
    int tx = (int)x + 26;
    int tb = (int)y + 52 + UI_F_BODY->ascent;
    ui_text_fit(fb, UI_F_BODY, tx, tb, n->title[0] ? n->title : n->app, gdim(C_PRI, fade), UI_LEFT, TEXT_W);
    if (n->body[0])
        ui_text_wrap(fb, UI_F_BODY, tx, tb + body_lh(), TEXT_W, body_lh(), 2, n->body, gdim(C_SEC, fade), UI_LEFT);
}

