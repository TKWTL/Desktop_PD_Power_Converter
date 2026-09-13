/* Product menu built on the MiaoUI core from Firmware_0.zip.  Page functions
 * live in APP/Pages/ (see dashboard.c), so this file only owns the menu tree. */
#include "ui_conf.h"
#include "core/ui.h"
#include "display/dispDriver.h"
#include "images/image.h"

#include "Pages/dashboard.h"
#include "Pages/service_pages.h"

static ui_page_t s_main_page;
static ui_page_t s_settings_page;

static ui_item_t s_dashboard_item;
static ui_item_t s_settings_item;
static ui_item_t s_settings_back_item;
static ui_item_t s_about_item;
static ui_item_t s_burnin_item;

void Create_Parameter(ui_t *ui)
{
    Create_Disp_Parameters(ui);
}

void Create_Text(ui_t *ui)
{
    static ui_text_t about_text;
    static ui_element_t about_element;

    (void)ui;

    about_text.ptr = "Desktop PD Power\nMiaoUI + u8g2\nSH1107 / SPI DMA\nAGPL-3.0-only";
    about_text.font = UI_FONT;
    about_text.fontHight = UI_FONT_HIGHT;
    about_text.fontWidth = UI_FONT_WIDTH;
    about_element.text = &about_text;
    Create_element(&s_about_item, &about_element);
}

void Create_MenuTree(ui_t *ui)
{
    (void)ui;

    AddPage("[Home]", &s_main_page, UI_PAGE_ICON, 0);
    AddItem("-Dashboard", UI_ITEM_WORD, img_dashboard,
            &s_dashboard_item, &s_main_page, 0, Dashboard_Page);

    AddItem("-Settings", UI_ITEM_PARENTS, img_configuration,
            &s_settings_item, &s_main_page, &s_settings_page, 0);
        AddPage("[Settings]", &s_settings_page, UI_PAGE_TEXT, &s_main_page);
        AddItem("[Home]", UI_ITEM_RETURN, 0,
                &s_settings_back_item, &s_settings_page, &s_main_page, 0);
        Add_Disp_Items(&s_settings_page);
        Add_Service_Items(&s_settings_page);

    AddItem("-Burn-in Test", UI_ITEM_WORD, img_burn_in,
            &s_burnin_item, &s_main_page, 0, Burnin_Page);

    AddItem("-About", UI_ITEM_WORD, img_user_account,
            &s_about_item, &s_main_page, 0, 0);
}

void MiaoUi_Setup(ui_t *ui)
{
    Create_UI(ui, &s_dashboard_item);

    /* Boot straight into the dashboard function page instead of painting the
     * icon menu first: ui_loop() only draws the menu from its UI_PAGE_INIT
     * branch, so starting in the item-running state keeps the menu as the page
     * the dashboard returns to. */
    ui->menuState = UI_ITEM_RUNING;
}
