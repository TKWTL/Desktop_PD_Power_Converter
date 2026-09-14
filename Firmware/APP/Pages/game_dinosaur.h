/* Dino runner page - ported from NUEDC_2025B Firmware_0
 * (APP/Applications/game_dinosaur.{c,h}); see the .c file for the porting
 * notes (pass based, fixed point, own rand(), 128x80 layout, procedural
 * ground due to u8g2's negative-coordinate drop). */
#ifndef APP_PAGES_GAME_DINOSAUR_H_
#define APP_PAGES_GAME_DINOSAUR_H_

#include "core/ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* MiaoUI function page: K1 (DOWN) jumps / restarts, K2 (ENTER) leaves. */
void Game_DinoSaur(ui_t *ui);

#ifdef __cplusplus
}
#endif

#endif /* APP_PAGES_GAME_DINOSAUR_H_ */
