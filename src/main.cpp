// NorseEvents - the Norse frameworks' shared event channel (UE4SS v4.0.0 C++ mod for NORSE: Oath of Blood), 2026-11-17.
//
// The game's moments run native-to-native (Phase 0: a ProcessEvent hook sees almost none of them), so each is caught by
// ONE native hook on the function's own implementation, found through the Norse Address Library by name, and told to
// every subscriber through the C API in include/NorseEvents.h. Modelled on powerofthree's Papyrus Extender events and on
// UE4SS's own warning that many mods hooking the same function separately slow the game: hook once, share.
//
// Hooked (NAL IDs): 2 ACombat_GM::OnUnitDeath -> UnitDied; 3 ACombat_GM::ExecuteAbility -> AbilityExecuted;
// 4 ACombat_GM::EndCombat and 5 ACombatArea::EndCombat -> FightEnded; 6 UUnitComponent::SetRemainingHealth_Internal ->
// UnitDamaged / UnitHealed. FightStarted is published by NorseRealtimeNative, which owns the combat-start hook.
// Console: ne.log on|off (one log line per event; default from NorseEvents.ini), ne.stats.
#include "NorseEvents.h"
#include "NorseAddressLibrary.hpp"
#include "Log.hpp"
#include "Ue.hpp"

#include <MinHook.h>

#include <atomic>
#include <map>
#include <set>
#include <mutex>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace nrn;
using namespace nrn::ue;

namespace
{
    struct Config
    {
        int LogLevel = 2;
        bool LogEvents = true;
        std::wstring LogDir;
    } g_cfg;
    fs::path g_mod, g_out;
    std::atomic<bool> g_logEvents{true};

    fs::path ModDir()
    {
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&ModDir), &self);
        wchar_t buf[MAX_PATH * 2]{};
        GetModuleFileNameW(self, buf, static_cast<DWORD>(std::size(buf)));
        return fs::path(buf).parent_path().parent_path();
    }

    void LoadConfig(const fs::path& ini)
    {
        std::ifstream in(ini);
        std::string line;
        while (std::getline(in, line))
        {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            size_t eq = line.find('=');
            if (line.empty() || line[0] == ';' || line[0] == '[' || eq == std::string::npos) continue;
            std::string k = line.substr(0, eq), v = line.substr(eq + 1);
            if (k == "uLogLevel") g_cfg.LogLevel = std::atoi(v.c_str());
            else if (k == "bLogEvents") g_cfg.LogEvents = std::atoi(v.c_str()) != 0;
            else if (k == "sLogDir")
            {
                int n = MultiByteToWideChar(CP_UTF8, 0, v.c_str(), -1, nullptr, 0);
                std::wstring w(static_cast<size_t>(n > 0 ? n - 1 : 0), L'\0');
                if (n > 1) MultiByteToWideChar(CP_UTF8, 0, v.c_str(), -1, w.data(), n);
                g_cfg.LogDir = w;
            }
        }
    }

    // ------------------------------------------------------------------ the channel
    struct Sub
    {
        int id;
        std::string name; // "*" = all
        NE_Callback cb;
        void* ctx;
    };
    std::recursive_mutex g_mx;
    std::vector<Sub> g_subs;
    int g_nextId = 1;
    std::map<std::string, uint64_t> g_counts;

    std::string Short(void* obj)
    {
        if (!obj) return "-";
        std::string n = Name(static_cast<UObject*>(obj));
        size_t d = n.find_last_of(".:");
        return d == std::string::npos ? n : n.substr(d + 1);
    }

    void Publish(const NE_Event& e)
    {
        if (!e.name) return;
        std::vector<Sub> subs;
        {
            std::lock_guard lk(g_mx);
            ++g_counts[e.name];
            subs = g_subs;
        }
        if (g_logEvents)
            NRN_INFO("EVENT %s: subject %s, other %s, asset %s, %d -> %d%s%s", e.name, Short(e.subject).c_str(), Short(e.other).c_str(),
                     Short(e.asset).c_str(), e.oldValue, e.newValue, (e.flags & NE_CRIT) ? ", crit" : "", (e.flags & NE_EXECUTED) ? ", executed" : "");
        for (const Sub& s : subs)
            if (s.name == "*" || s.name == e.name)
            {
                try
                {
                    s.cb(&e, s.ctx);
                }
                catch (...)
                {
                    NRN_ERROR("subscriber %d threw on %s", s.id, e.name);
                }
            }
    }

    NE_Event Make(const char* name, void* subject, void* other, void* asset, int32_t a = 0, int32_t b = 0, uint32_t flags = 0)
    {
        return NE_Event{static_cast<uint32_t>(sizeof(NE_Event)), name, subject, other, asset, a, b, flags};
    }

    // ------------------------------------------------------------------ hooks
    using OnUnitDeathFn = void(__fastcall*)(UObject* gm, UObject* killer, UObject* victim, bool executed, UObject* ability);
    using ExecuteAbilityFn = void(__fastcall*)(UObject* gm, const uint8_t* unit, const uint8_t* target, UObject* ability);
    using EndCombatFn = void(__fastcall*)(UObject* self);
    using SetHealthFn = void(__fastcall*)(UObject* uc, int32_t health, UObject* cause, bool crit, UObject* ability);

    OnUnitDeathFn o_OnUnitDeath = nullptr;
    ExecuteAbilityFn o_ExecuteAbility = nullptr;
    EndCombatFn o_EndCombatGM = nullptr, o_EndCombatArea = nullptr;
    SetHealthFn o_SetHealth = nullptr;

    // First run: the game calls OnUnitDeath twice for one death (once as health reaches 0, once from Die), and a lethal
    // hit's death came before its UnitDamaged (the death runs inside the health change). So: one UnitDied per unit until
    // it lives again (healed, or a new fight), and deaths raised inside a health change wait until after its UnitDamaged.
    std::set<void*> g_dead;
    int g_inHealth = 0;
    std::vector<NE_Event> g_pending;

    void __fastcall OnUnitDeathHook(UObject* gm, UObject* killer, UObject* victim, bool executed, UObject* ability)
    {
        o_OnUnitDeath(gm, killer, victim, executed, ability);
        if (!g_dead.insert(victim).second)
        {
            NRN_DEBUG("UnitDied: second OnUnitDeath for %s - not published again", Short(victim).c_str());
            return;
        }
        NE_Event e = Make("UnitDied", victim, killer, ability, 0, 0, executed ? NE_EXECUTED : 0);
        if (g_inHealth) g_pending.push_back(e);
        else Publish(e);
    }

    // FUnitPlacement (16 bytes, passed by pointer): FGridPlacement 0..8, UObject* Unit @8
    UObject* UnitOf(const uint8_t* placement)
    {
        if (!placement) return nullptr;
        UObject* u = nullptr;
        std::memcpy(&u, placement + 8, 8);
        return u;
    }

    void __fastcall ExecuteAbilityHook(UObject* gm, const uint8_t* unit, const uint8_t* target, UObject* ability)
    {
        UObject* u = UnitOf(unit);
        UObject* t = UnitOf(target);
        o_ExecuteAbility(gm, unit, target, ability);
        Publish(Make("AbilityExecuted", u, t, ability));
    }

    void __fastcall EndCombatGMHook(UObject* self)
    {
        o_EndCombatGM(self);
        Publish(Make("FightEnded", self, nullptr, nullptr));
    }

    void __fastcall EndCombatAreaHook(UObject* self)
    {
        o_EndCombatArea(self);
        Publish(Make("FightEnded", self, nullptr, nullptr));
    }

    void __fastcall SetHealthHook(UObject* uc, int32_t health, UObject* cause, bool crit, UObject* ability)
    {
        int32_t before = 0;
        if (auto* hp = Reflect::Get().Prop<int32_t>(uc, L"RemainingHealth")) before = *hp;
        ++g_inHealth;
        o_SetHealth(uc, health, cause, crit, ability);
        --g_inHealth;
        int32_t after = health;
        if (auto* hp = Reflect::Get().Prop<int32_t>(uc, L"RemainingHealth")) after = *hp;
        if (after != before)
        {
            UObject* unit = GetObjectRet(uc, L"GetOwner");
            if (after > 0 && unit) g_dead.erase(unit); // alive again: a later death is a new one
            Publish(Make(after < before ? "UnitDamaged" : "UnitHealed", unit ? unit : uc, cause, ability, before, after, crit ? NE_CRIT : 0));
        }
        if (!g_inHealth && !g_pending.empty())
        {
            std::vector<NE_Event> p;
            p.swap(g_pending);
            for (const NE_Event& e : p) Publish(e);
        }
    }

    int g_hooked = 0;

    template <typename F>
    void HookId(uint32_t id, const char* what, F detour, F& original)
    {
        auto target = reinterpret_cast<void*>(nal::Library::Get().Address(id));
        if (!target)
        {
            NRN_WARN("%s: NAL ID %u not resolved - that event stays off", what, id);
            return;
        }
        if (MH_CreateHook(target, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(&original)) != MH_OK ||
            MH_EnableHook(target) != MH_OK)
        {
            NRN_ERROR("%s: hook failed at %p - that event stays off", what, target);
            original = nullptr;
            return;
        }
        ++g_hooked;
        NRN_INFO("%s: hooked (NAL ID %u, %s)", what, id, nal::Library::Get().NameOf(reinterpret_cast<uintptr_t>(target)).c_str());
    }

    void InstallHooks()
    {
        // ue4ss\NorseAddressLibrary - two levels up from this mod folder (the first run looked in Mods and found nothing)
        fs::path nalDir = g_mod.parent_path().parent_path() / "NorseAddressLibrary";
        if (!nal::Library::Get().Load(nalDir, [](const std::string& m) { NRN_INFO("%s", m.c_str()); }))
        {
            NRN_ERROR("Norse Address Library not found at %s - no game events (UnitDied, UnitDamaged...) this session",
                      Narrow(nalDir.wstring()).c_str());
            return;
        }
        MH_STATUS s = MH_Initialize();
        if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED)
        {
            NRN_ERROR("MinHook init failed (%d)", static_cast<int>(s));
            return;
        }
        HookId(nal::ID::ACombat_GM::OnUnitDeath, "UnitDied", &OnUnitDeathHook, o_OnUnitDeath);
        HookId(nal::ID::ACombat_GM::ExecuteAbility, "AbilityExecuted", &ExecuteAbilityHook, o_ExecuteAbility);
        HookId(nal::ID::ACombat_GM::EndCombat, "FightEnded (game mode)", &EndCombatGMHook, o_EndCombatGM);
        HookId(nal::ID::ACombatArea::EndCombat, "FightEnded (combat area)", &EndCombatAreaHook, o_EndCombatArea);
        HookId(nal::ID::UUnitComponent::SetRemainingHealth_Internal, "UnitDamaged/UnitHealed", &SetHealthHook, o_SetHealth);
        NRN_INFO("hooks: %d of 5 installed", g_hooked);
    }

    bool RunCommand(const std::wstring& c)
    {
        std::wstring cmd = c;
        for (auto& ch : cmd) ch = static_cast<wchar_t>(towlower(ch));
        if (cmd == L"ne.log on" || cmd == L"ne.log off")
        {
            g_logEvents = cmd == L"ne.log on";
            NRN_INFO("ne.log: event lines %s", g_logEvents ? "on" : "off");
        }
        else if (cmd == L"ne.stats")
        {
            std::lock_guard lk(g_mx);
            NRN_INFO("ne.stats: %zu subscriber(s), %d hook(s)", g_subs.size(), g_hooked);
            for (auto& [n, k] : g_counts) NRN_INFO("  %s: %llu", n.c_str(), static_cast<unsigned long long>(k));
        }
        else NRN_WARN("unknown command - ne.log on|off | ne.stats");
        return true;
    }
} // namespace

// ------------------------------------------------------------------ the C API (include/NorseEvents.h)
extern "C" __declspec(dllexport) int NE_Subscribe(const char* name, NE_Callback cb, void* ctx)
{
    if (!name || !cb) return 0;
    std::lock_guard lk(g_mx);
    int id = g_nextId++;
    g_subs.push_back({id, name, cb, ctx});
    NRN_INFO("subscriber %d: %s", id, name);
    return id;
}

extern "C" __declspec(dllexport) void NE_Unsubscribe(int id)
{
    std::lock_guard lk(g_mx);
    g_subs.erase(std::remove_if(g_subs.begin(), g_subs.end(), [&](const Sub& s) { return s.id == id; }), g_subs.end());
}

extern "C" __declspec(dllexport) void NE_Publish(const NE_Event* e)
{
    if (!e || e->size < offsetof(NE_Event, flags) + sizeof(uint32_t)) return;
    if (e->name && std::strcmp(e->name, "FightStarted") == 0) g_dead.clear(); // a new fight: every unit may die once more
    Publish(*e);
}

class NorseEvents : public RC::CppUserModBase
{
  public:
    NorseEvents()
    {
        ModName = L"NorseEvents";
        ModVersion = L"" NE_VERSION;
        ModDescription = L"The shared event channel: hooks each game moment once (death, damage, ability, fight end) and tells every subscriber.";
        ModAuthors = L"ApocryphaRealm";
        g_mod = ModDir();
        LoadConfig(g_mod / "NorseEvents.ini");
        g_logEvents = g_cfg.LogEvents;
        g_out = g_cfg.LogDir.empty() ? g_mod : fs::path(g_cfg.LogDir);
        Log::Get().Open(g_out / "NorseEvents.log", g_cfg.LogLevel);
        Log::Get().Write(Level::Error, "NorseEvents v%s - log level %d, event lines %s", NE_VERSION, g_cfg.LogLevel, g_cfg.LogEvents ? "on" : "off");
    }

    auto on_unreal_init() -> void override
    {
        NRN_INFO("on_unreal_init: installing hooks and the console callback");
        InstallHooks();
        try
        {
            RC::Unreal::Hook::RegisterProcessConsoleExecCallback(
                    [](UObject*, const wchar_t* cmd, RC::Unreal::FOutputDevice&, UObject*) -> bool {
                        if (!cmd || _wcsnicmp(cmd, L"ne.", 3) != 0) return false;
                        static ULONGLONG last = 0;
                        static std::wstring lastCmd;
                        ULONGLONG now = GetTickCount64();
                        if (lastCmd == cmd && now - last < 250) return true;
                        last = now;
                        lastCmd = cmd;
                        return RunCommand(cmd);
                    });
        }
        catch (const std::exception& e)
        {
            NRN_ERROR("console callback registration failed: %s", e.what());
        }
    }
};

extern "C" __declspec(dllexport) RC::CppUserModBase* start_mod()
{
    return new NorseEvents();
}

extern "C" __declspec(dllexport) void uninstall_mod(RC::CppUserModBase* mod)
{
    delete mod;
}
