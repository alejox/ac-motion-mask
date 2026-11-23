/*
AC Motion Mask - frontend hooks: motion engine, Tools menu entry and updater
Copyright (C) 2026 Alejandro Grajales (alejoxcode)
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "updater.hpp"

extern "C" {
#include "motion-engine.h"
}

#include <obs-frontend-api.h>
#include <obs-module.h>

extern "C" void acm_frontend_load(void)
{
	acm_engine_init();
	obs_frontend_add_tools_menu_item(
		obs_module_text("Update.CheckMenu"), [](void *) { acm::Updater::instance().check(true); }, nullptr);
	acm::Updater::instance().start();
}

extern "C" void acm_frontend_unload(void)
{
	acm::Updater::instance().stop();
	acm_engine_free();
}
