/** vim: set ts=4 sw=4 et tw=99:
 *
 * === Stripper for Metamod:Source ===
 * Copyright (C) 2005-2009 David "BAILOPAN" Anderson
 * No warranties of any kind.
 * Based on the original concept of Stripper2 by botman
 *
 * License: see LICENSE.TXT
 * ===================================
 */

#include <stdio.h>
#include <stddef.h>
#ifdef WIN32
#include <windows.h>
#define snprintf _snprintf
#define vsnprintf _vsnprintf
#endif
#include "stripper_mm.h"
#include "intercom.h"
#include "icommandline.h"
#include <stripper_version_auto.h>

using namespace SourceHook;

StripperPlugin g_Plugin;

PLUGIN_EXPOSE(StripperPlugin, g_Plugin);

IVEngineServer *engine = NULL;
IServerGameDLL *server = NULL;
IServerGameClients *clients = NULL;
ICvar *icvar = NULL;
static SourceHook::String g_mapname;
static stripper_core_t stripper_core;
static char game_path[256];
static char stripper_path[256];
static char stripper_cfg_path[256];

SH_DECL_HOOK0(IVEngineServer, GetMapEntitiesString, SH_NOATTRIB, 0, const char *);
SH_DECL_HOOK6(IServerGameDLL, LevelInit, SH_NOATTRIB, 0, bool, char const *, char const *, char const *, char const *, bool, bool);
SH_DECL_HOOK1_void(IServerGameClients, SetCommandClient, SH_NOATTRIB, 0, int);

#if defined WIN32
#define dlopen(x, y) LoadLibrary(x)
#define dlclose(x) FreeLibrary(x)
#define dlsym(x, y) GetProcAddress(x, y)
typedef HMODULE LibraryHandle;
#define PATH_SEP_STR "\\"
#else
typedef void *LibraryHandle;
#define PATH_SEP_STR "/"
#endif

#if defined(_WIN64) || defined(__x86_64__)
#define PLATFORM_ARCH_FOLDER "x64" PATH_SEP_STR
#else
#define PLATFORM_ARCH_FOLDER ""
#endif

static LibraryHandle stripper_lib;

static void
SetCommandClient(int client);

/**
 * Something like this is needed to register cvars/CON_COMMANDs.
 */
class BaseAccessor : public IConCommandBaseAccessor
{
public:
	bool RegisterConCommandBase(ConCommandBase *pCommandBase)
	{
		/* Always call META_REGCVAR instead of going through the engine. */
		return META_REGCVAR(pCommandBase);
	}
} s_BaseAccessor;

static void
log_message(const char *fmt, ...)
{
	va_list ap;
	char buffer[1024];

	va_start(ap, fmt);
	vsnprintf(buffer, sizeof(buffer), fmt, ap);
	va_end(ap);

	buffer[sizeof(buffer) - 1] = '\0';

	g_SMAPI->LogMsg(g_PLAPI, "%s", buffer);
}

static void
path_format(char *buffer, size_t maxlength, const char *fmt, ...)
{
	va_list ap;
	char new_buffer[1024];

	va_start(ap, fmt);
	vsnprintf(new_buffer, sizeof(new_buffer), fmt, ap);
	va_end(ap);

	new_buffer[sizeof(new_buffer) - 1] = '\0';

	g_SMAPI->PathFormat(buffer, maxlength, "%s", new_buffer);
}

static const char *
get_map_name()
{
	return STRING(g_SMAPI->GetCGlobals()->mapname);
}

static stripper_game_t stripper_game =
	{
		NULL,
		NULL,
		NULL,
		log_message,
		path_format,
		get_map_name,
};

ConVar cvar_stripper_cfg_path("stripper_cfg_path", "addons/stripper", FCVAR_NONE, "Stripper Config Path");

ConVar stripper_curfile("stripper_current_file", "", FCVAR_SPONLY | FCVAR_NOTIFY, "Stripper for current map");

ConVar stripper_nextfile("stripper_next_file", "", FCVAR_PROTECTED | FCVAR_SPONLY, "Stripper for next map");

ConVar stripper_lowercase("stripper_file_lowercase", "0", FCVAR_NONE, "Load stripper configs in lowercase");

#if SOURCE_ENGINE >= SE_ORANGEBOX
void stripper_cfg_path_changed(IConVar *var, const char *pOldValue, float flOldValue)
#else
void stripper_cfg_path_changed(ConVar *var, const char *pOldValue)
#endif
{
	strncpy(stripper_cfg_path, cvar_stripper_cfg_path.GetString(), sizeof(stripper_cfg_path));
}

bool StripperPlugin::Load(PluginId id, ISmmAPI *ismm, char *error, size_t maxlen, bool late)
{
	PLUGIN_SAVEVARS();

	GET_V_IFACE_ANY(GetServerFactory, server, IServerGameDLL, INTERFACEVERSION_SERVERGAMEDLL);
#if SOURCE_ENGINE == SE_SDK2013
	// Shim to avoid hooking shims
	engine = (IVEngineServer *)ismm->GetEngineFactory()("VEngineServer023", nullptr);
	if (!engine)
	{
		engine = (IVEngineServer *)ismm->GetEngineFactory()("VEngineServer022", nullptr);
		if (!engine)
		{
			engine = (IVEngineServer *)ismm->GetEngineFactory()("VEngineServer021", nullptr);
			if (!engine)
			{
				if (error && maxlen)
				{
					ismm->Format(error, maxlen, "Could not find interface: VEngineServer023 or VEngineServer022 or VEngineServer021");
				}
				return false;
			}
		}
	}
#else
	GET_V_IFACE_CURRENT(GetEngineFactory, engine, IVEngineServer, INTERFACEVERSION_VENGINESERVER);
#endif
	GET_V_IFACE_CURRENT(GetServerFactory, clients, IServerGameClients, INTERFACEVERSION_SERVERGAMECLIENTS);
	GET_V_IFACE_CURRENT(GetEngineFactory, icvar, ICvar, CVAR_INTERFACE_VERSION);

	engine->GetGameDir(game_path, sizeof(game_path));
	stripper_game.game_path = game_path;
	stripper_game.stripper_path = "addons/stripper";
	stripper_game.stripper_cfg_path = stripper_cfg_path;
	strncpy(stripper_cfg_path, cvar_stripper_cfg_path.GetString(), sizeof(stripper_cfg_path));

	cvar_stripper_cfg_path.InstallChangeCallback(stripper_cfg_path_changed);

#if SOURCE_ENGINE == SE_DARKMESSIAH
	const char *temp = (icvar == NULL) ? NULL : icvar->GetCommandLineValue("+stripper_path");
#else
	const char *temp = CommandLine()->ParmValue("+stripper_path");
#endif
	if (temp != NULL && temp[0] != '\0')
	{
		g_SMAPI->PathFormat(stripper_path, sizeof(stripper_path), "%s", temp);
		stripper_game.stripper_path = stripper_path;
	}

#if defined __linux__
#define PLATFORM_EXT ".so"
#elif defined __APPLE__
#define PLATFORM_EXT ".dylib"
#else
#define PLATFORM_EXT ".dll"
#endif

	char path[256];
	g_SMAPI->PathFormat(path,
						sizeof(path),
						"%s/%s/bin/" PLATFORM_ARCH_FOLDER "stripper.core" PLATFORM_EXT,
						game_path,
						stripper_game.stripper_path);

#undef PLATFORM_EXT

	stripper_lib = dlopen(path, RTLD_NOW);
	if (stripper_lib == NULL)
	{
#if defined __linux__ || defined __APPLE__
		snprintf(error, maxlen, "%s", dlerror());
#elif defined WIN32
		DWORD dw = GetLastError();
		if (FormatMessageA(
				FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
				NULL,
				dw,
				MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
				error,
				maxlen,
				NULL) == 0)
		{
			_snprintf(error, maxlen, "Unknown error: %d", dw);
			error[maxlen - 1] = '\0';
		}
#endif
		return false;
	}

	STRIPPER_LOAD stripper_load = (STRIPPER_LOAD)dlsym(stripper_lib, "LoadStripper");
	if (stripper_load == NULL)
	{
		dlclose(stripper_lib);
		stripper_load = NULL;
		snprintf(error, maxlen, "Could not find LoadStripper function");
		error[maxlen - 1] = '\0';
		return false;
	}

	stripper_load(&stripper_game, &stripper_core);

	SH_ADD_HOOK_STATICFUNC(IVEngineServer, GetMapEntitiesString, engine, GetMapEntitiesString_handler, false);
	SH_ADD_HOOK_STATICFUNC(IServerGameDLL, LevelInit, server, LevelInit_handler, false);
	SH_ADD_HOOK_STATICFUNC(IServerGameClients, SetCommandClient, clients, SetCommandClient, false);

#if SOURCE_ENGINE >= SE_ORANGEBOX
	g_pCVar = icvar;
	ConVar_Register(0, &s_BaseAccessor);
#else
	ConCommandBaseMgr::OneTimeInit(&s_BaseAccessor);
#endif

	return true;
}

bool StripperPlugin::Unload(char *error, size_t maxlen)
{
	SH_REMOVE_HOOK_STATICFUNC(IVEngineServer, GetMapEntitiesString, engine, GetMapEntitiesString_handler, false);
	SH_REMOVE_HOOK_STATICFUNC(IServerGameDLL, LevelInit, server, LevelInit_handler, false);
	SH_REMOVE_HOOK_STATICFUNC(IServerGameClients, SetCommandClient, clients, SetCommandClient, false);
	stripper_core.unload();
	dlclose(stripper_lib);
	stripper_lib = NULL;

	return true;
}

const char *
GetMapEntitiesString_handler()
{
	RETURN_META_VALUE(MRES_SUPERCEDE, stripper_core.ent_string());
}

bool LevelInit_handler(char const *pMapName, char const *pMapEntities, char const *c, char const *d, bool e, bool f)
{
	if (strlen(stripper_nextfile.GetString()) > 0)
	{
		g_mapname.assign(stripper_nextfile.GetString());
		log_message("Loading %s for map \"%s\"", g_mapname.c_str(), pMapName);
	}
	else if (stripper_lowercase.GetInt())
	{
		char *name = UTIL_ToLowerCase(pMapName);
		g_mapname.assign(name);
		delete[] name;
	}
	else
	{
		g_mapname.assign(pMapName);
	}

	stripper_nextfile.SetValue("");
	stripper_curfile.SetValue(g_mapname.c_str());

	const char *ents = stripper_core.parse_map(g_mapname.c_str(), pMapEntities);
	RETURN_META_VALUE_NEWPARAMS(MRES_IGNORED, true, &IServerGameDLL::LevelInit, (pMapName, ents, c, d, e, f));
}

char *
UTIL_ToLowerCase(const char *str)
{
	size_t len = strlen(str);
	char *buffer = new char[len + 1];
	for (size_t i = 0; i < len; i++)
	{
		if (str[i] >= 'A' && str[i] <= 'Z')
			buffer[i] = str[i] - ('A' - 'a');
		else
			buffer[i] = str[i];
	}
	buffer[len] = '\0';
	return buffer;
}

bool StripperPlugin::Pause(char *error, size_t maxlen)
{
	return true;
}

bool StripperPlugin::Unpause(char *error, size_t maxlen)
{
	return true;
}

void StripperPlugin::AllPluginsLoaded()
{
}

const char *
StripperPlugin::GetAuthor()
{
	return "BAILOPAN";
}

const char *
StripperPlugin::GetName()
{
	return "Stripper";
}

const char *
StripperPlugin::GetDescription()
{
	return "Strips/Adds Map Entities";
}

const char *
StripperPlugin::GetURL()
{
	return "http://www.bailopan.net/";
}

const char *
StripperPlugin::GetLicense()
{
	return "GPL v3";
}

const char *
StripperPlugin::GetVersion()
{
	return STRIPPER_FULL_VERSION;
}

const char *
StripperPlugin::GetDate()
{
	return __DATE__;
}

const char *
StripperPlugin::GetLogTag()
{
	return "STRIPPER";
}

static int last_command_client = 1;

static void
SetCommandClient(int client)
{
	last_command_client = client;
}

ConVar stripper_version("stripper_version", STRIPPER_FULL_VERSION, FCVAR_SPONLY | FCVAR_NOTIFY, "Stripper Version");

CON_COMMAND(stripper_dump, "Dumps the map entity list to a file")
{
	if (last_command_client != -1)
		return;

	stripper_core.command_dump();
}
