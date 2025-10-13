#ifndef UI_H
#define UI_H

#include <cstdint>

bool RenderUI();

constexpr int JOIN_VALUE_BUF_SIZE = 128;
constexpr int JOIN_JOBID_BUF_SIZE = 128;

extern char join_value_buf[JOIN_VALUE_BUF_SIZE];
extern char join_jobid_buf[JOIN_JOBID_BUF_SIZE];

extern int join_type_combo_index;
extern int g_activeTab;

enum Tab {
	Tab_Accounts,
	Tab_Friends,
	Tab_Servers,
	Tab_Games,
	Tab_History,
	Tab_Console,
	Tab_Settings,
	Tab_Inventory,
	Tab_COUNT
};

extern uint64_t g_targetPlaceId_ServersTab;
extern uint64_t g_targetUniverseId_ServersTab;

#endif
