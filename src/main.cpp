#include "generated/pg_init.h"
#include "pg_app.h"

#if REX_PLATFORM_WIN32
#include <windows.h>
// SDL reads hints from the environment when it starts: use the executable's icon (resource 1)
// for the window too.
static const bool kWindowIconHint =
    SetEnvironmentVariableW(L"SDL_WINDOWS_INTRESOURCE_ICON", L"1") != 0;
#endif

static std::unique_ptr<rex::ui::WindowedApp> CreatePgApp(
    rex::ui::WindowedAppContext& ctx) {
  // Keep the flags and module registration emitted by the current code generator.
  return PgApp::Create(ctx, PPCImageConfig);
}

REX_DEFINE_APP(pg, CreatePgApp)
