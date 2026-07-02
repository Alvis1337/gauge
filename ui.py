"""
Reusable UI primitives for 480x320 touchscreen.
All drawing targets a pygame.Surface; touch coords come from touch.TouchController.
"""
import pygame

W, H = 480, 320

# ── palette ──────────────────────────────────────────────────────────────────
BG       = (17,  17,  17)
PANEL    = (30,  30,  30)
CARD     = (40,  40,  40)
ACCENT   = (68, 170, 255)
DANGER   = (255, 68,  68)
SUCCESS  = (68, 220, 100)
TEXT     = (255, 255, 255)
SUBTEXT  = (160, 160, 160)
BORDER   = (60,  60,  60)

# ── fonts ─────────────────────────────────────────────────────────────────────
pygame.font.init()
_F_SM  = pygame.font.SysFont("monospace", 14)
_F_MD  = pygame.font.SysFont("monospace", 18)
_F_LG  = pygame.font.SysFont("monospace", 22, bold=True)
_F_XL  = pygame.font.SysFont("monospace", 28, bold=True)


def text(surf, msg, x, y, font=_F_MD, color=TEXT, anchor="topleft"):
    s = font.render(str(msg), True, color)
    r = s.get_rect(**{anchor: (x, y)})
    surf.blit(s, r)
    return r


def rect_btn(surf, label, rect, color=ACCENT, text_color=TEXT, font=_F_MD, radius=6):
    """Draw a filled rounded-rect button; return the Rect."""
    r = pygame.Rect(rect)
    pygame.draw.rect(surf, color, r, border_radius=radius)
    s = font.render(label, True, text_color)
    surf.blit(s, s.get_rect(center=r.center))
    return r


def outline_btn(surf, label, rect, color=BORDER, text_color=TEXT, font=_F_MD, radius=6):
    r = pygame.Rect(rect)
    pygame.draw.rect(surf, PANEL, r, border_radius=radius)
    pygame.draw.rect(surf, color, r, width=1, border_radius=radius)
    s = font.render(label, True, text_color)
    surf.blit(s, s.get_rect(center=r.center))
    return r


def hit(rect, pos) -> bool:
    return pos is not None and pygame.Rect(rect).collidepoint(pos)


# ── on-screen keyboard ────────────────────────────────────────────────────────
_ROWS = [
    list("1234567890"),
    list("qwertyuiop"),
    list("asdfghjkl."),
    list("zxcvbnm_-@"),
]
_KEY_W, _KEY_H = 42, 36
_KB_X = (W - len(_ROWS[0]) * _KEY_W) // 2
_KB_Y = 130


def draw_keyboard(surf, shift=False):
    for r, row in enumerate(_ROWS):
        for c, ch in enumerate(row):
            label = ch.upper() if shift else ch
            kx = _KB_X + c * _KEY_W
            ky = _KB_Y + r * _KEY_H
            rect_btn(surf, label, (kx + 1, ky + 1, _KEY_W - 2, _KEY_H - 2),
                     color=CARD, text_color=TEXT, font=_F_SM)
    # Special keys
    rect_btn(surf, "⌫",  (_KB_X,              _KB_Y + 4 * _KEY_H + 2, 80, _KEY_H - 2), DANGER,  font=_F_MD)
    rect_btn(surf, "⇧",  (_KB_X + 84,         _KB_Y + 4 * _KEY_H + 2, 80, _KEY_H - 2), PANEL,   font=_F_MD)
    rect_btn(surf, "SPC", (_KB_X + 168,        _KB_Y + 4 * _KEY_H + 2, 120, _KEY_H - 2), CARD,  font=_F_SM)
    rect_btn(surf, "OK",  (_KB_X + 292,        _KB_Y + 4 * _KEY_H + 2, 128, _KEY_H - 2), SUCCESS, font=_F_MD)


def keyboard_hit(pos, shift=False):
    """Return typed char, 'BACKSPACE', 'SHIFT', 'SPACE', 'OK', or None."""
    if pos is None:
        return None
    px, py = pos
    for r, row in enumerate(_ROWS):
        for c, ch in enumerate(row):
            kx = _KB_X + c * _KEY_W
            ky = _KB_Y + r * _KEY_H
            if pygame.Rect(kx + 1, ky + 1, _KEY_W - 2, _KEY_H - 2).collidepoint(px, py):
                return ch.upper() if shift else ch
    # Special keys
    by = _KB_Y + 4 * _KEY_H + 2
    if pygame.Rect(_KB_X,       by, 80,  _KEY_H - 2).collidepoint(px, py): return "BACKSPACE"
    if pygame.Rect(_KB_X + 84,  by, 80,  _KEY_H - 2).collidepoint(px, py): return "SHIFT"
    if pygame.Rect(_KB_X + 168, by, 120, _KEY_H - 2).collidepoint(px, py): return "SPACE"
    if pygame.Rect(_KB_X + 292, by, 128, _KEY_H - 2).collidepoint(px, py): return "OK"
    return None


# ── number-pad ────────────────────────────────────────────────────────────────
_PAD_KEYS = [
    ["1","2","3"],
    ["4","5","6"],
    ["7","8","9"],
    [".","0","⌫"],
]
_PAD_W, _PAD_H = 70, 38
_PAD_X, _PAD_Y = (W - 3 * _PAD_W) // 2, 88


def draw_numpad(surf):
    for r, row in enumerate(_PAD_KEYS):
        for c, ch in enumerate(row):
            kx = _PAD_X + c * _PAD_W
            ky = _PAD_Y + r * _PAD_H
            color = DANGER if ch == "⌫" else CARD
            rect_btn(surf, ch, (kx + 2, ky + 2, _PAD_W - 4, _PAD_H - 4),
                     color=color, font=_F_LG)
    rect_btn(surf, "OK", (_PAD_X, _PAD_Y + 4 * _PAD_H + 4, 3 * _PAD_W - 4, _PAD_H - 4),
             color=SUCCESS, font=_F_LG)


def numpad_hit(pos):
    """Return digit/'.', 'BACKSPACE', 'OK', or None."""
    if pos is None:
        return None
    px, py = pos
    for r, row in enumerate(_PAD_KEYS):
        for c, ch in enumerate(row):
            kx = _PAD_X + c * _PAD_W
            ky = _PAD_Y + r * _PAD_H
            if pygame.Rect(kx + 2, ky + 2, _PAD_W - 4, _PAD_H - 4).collidepoint(px, py):
                return "BACKSPACE" if ch == "⌫" else ch
    oy = _PAD_Y + 4 * _PAD_H + 4
    if pygame.Rect(_PAD_X, oy, 3 * _PAD_W - 4, _PAD_H - 4).collidepoint(px, py):
        return "OK"
    return None
