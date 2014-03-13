#include <iostream>
#include <cstdlib>
#include <string>
#include <array>
#include <vector>
#include <ncurses.h>
// debug
#include <fstream>
// end debug
#include "qemu_manage.h"

static const std::string vmdir_regex = "^vmdir\\s*=\\s*\"(.*)\"";
static const std::string lmax_regex = "^list_max\\s*=\\s*\"(.*)\"";
static const std::string cfg = "test.cfg";
static const std::string dbf = "/var/db/qemu_manage2.db";
static const std::string sql_list_vm = "select name from vms order by name asc";

#ifdef ENABLE_OPENVSWITCH
#define CHOICES_NUM 3
#else
#define CHOICES_NUM 2
#endif
const std::array<std::string, CHOICES_NUM> choices = {
  "Manage qemu VM",
  "Add qemu VM",
#ifdef ENABLE_OPENVSWITCH
  "Show network map"
#endif
};
#undef CHOICES_NUM

int main() {

  using namespace QManager;

  uint32_t highlight(1);
  uint32_t ch;

  initscr();
  raw();
  noecho();
  curs_set(0);

  MainWindow *main_window = new MainWindow(10, 30);
  main_window->Init();

  std::string vmdir = get_param(cfg, vmdir_regex);

  for (;;) {
    uint32_t choice(0);

    main_window->Print();
    MenuList *main_menu = new MenuList(main_window->window, highlight);
    main_menu->Print(choices.begin(), choices.end());

    while((ch = wgetch(main_window->window))) {
      switch(ch) {
        case KEY_UP:
          if (highlight == 1)
            highlight = choices.size();
          else
            highlight--;
          break;
        case KEY_DOWN:
          if (highlight == choices.size())
            highlight = 1;
          else
            highlight++;
          break;
        case 10:
          choice = highlight;
          break;
        case KEY_F(10):
          delete main_menu;
          delete main_window;
          clear();
          refresh();
          endwin();
          exit(0);
          break;
      }

      MenuList *main_menu = new MenuList(main_window->window, highlight);
      main_menu->Print(choices.begin(), choices.end());

      if(choice != 0)
        break;
    }
    if(choice == MenuVmlist) {

      QemuDb *db = new QemuDb(dbf);
      VectorString res = db->SelectQuery(sql_list_vm);
      delete db;

      if(res.empty()) {
        PopupWarning *Warn = new PopupWarning("No guests here.", 3, 20, 7, 31);
        Warn->Init();
        Warn->Print(Warn->window);
      }
      else {

        std::string listmax_s = get_param(cfg, lmax_regex);
        uint32_t listmax;

        if(listmax_s.empty()) {
          listmax = 10;
        }
        else {
          listmax = std::stoi(listmax_s);
        }
      }

    }
    else if(choice == MenuAddVm) {
      // do some stuff...
    }
#ifdef ENABLE_OPENVSWITCH
    else if(choice == MenuOvsMap) {
      // do some stuff...
    }
#endif
  }

  return 0;
}
