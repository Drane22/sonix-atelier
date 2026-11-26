#ifndef STARTUP_ANIM_H
#define STARTUP_ANIM_H

#include <stdbool.h>

#include "src/gui/shell/gui.h"

bool startup_anim_enabled(void);
void startup_anim_set_enabled(bool enabled);

bool startup_anim_should_run(bool first_boot_wizard_active);
bool startup_anim_start(const gui_config_t *cfg, bool first_boot_wizard_active);
bool startup_anim_start_preview(const gui_config_t *cfg);
bool startup_anim_is_active(void);
void startup_anim_cancel(void);

void startup_anim_reset_boot_flag_for_test(void);

#endif // STARTUP_ANIM_H
