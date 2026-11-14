#ifndef __PLATFORM_AMIGA_GAME_INPUT_H
#define __PLATFORM_AMIGA_GAME_INPUT_H

void game_input_apply_tick(void);
int game_input_quit_requested(void);
void game_input_describe(char *text, int size);  /* INPUTTEST */

#endif
