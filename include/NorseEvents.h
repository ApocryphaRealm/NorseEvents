// NorseEvents.h - the public API of NorseEvents (the Norse frameworks' shared event channel), 2026-11-17.
//
// NorseEvents hooks each game moment ONCE and tells every subscriber; a mod subscribes instead of hooking the same game
// function again. Plain C exports, so any mod (any compiler) can use them:
//
//   #include "NorseEvents.h"
//   static void OnDeath(const NE_Event* e, void* ctx) { ... e->subject (victim), e->other (killer) ... }
//   if (auto* api = ne::Api()) api->Subscribe("UnitDied", &OnDeath, nullptr);        // "*" = every event
//
// Events (v0.1): UnitDamaged, UnitHealed (subject = the unit, other = the cause, asset = the ability, oldValue/newValue =
// health, flags & NE_CRIT), UnitDied (subject = victim, other = killer, asset = ability, flags & NE_EXECUTED),
// AbilityExecuted (subject = unit, other = target, asset = ability), FightStarted (subject = the CombatArea; published by
// NorseRealtimeNative, which owns that hook), FightEnded (subject = the CombatArea or game mode).
// Callbacks run on the game thread, inside the game's own call: keep them short, never block.
#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>

extern "C"
{
    enum NE_Flags : uint32_t
    {
        NE_CRIT = 1u << 0,
        NE_EXECUTED = 1u << 1,
    };

    struct NE_Event
    {
        uint32_t size;    // sizeof(NE_Event) of the publisher - fields may be added at the end later
        const char* name; // event name
        void* subject;    // UObject*: the unit the event is about
        void* other;      // UObject*: the other party (killer, cause, target), or null
        void* asset;      // UObject*: the ability asset involved, or null
        int32_t oldValue;
        int32_t newValue;
        uint32_t flags;
        const char* text; // 0.1.0 later (NorsePayload): a payload's name, or null. Read only when size covers it:
                          // size >= offsetof(NE_Event, text) + sizeof(const char*) - see NE_Text()
    };

    inline const char* NE_Text(const NE_Event* e)
    {
        return (e && e->size >= offsetof(NE_Event, text) + sizeof(const char*)) ? e->text : nullptr;
    }

    typedef void (*NE_Callback)(const NE_Event* e, void* ctx);
    typedef int (*NE_SubscribeFn)(const char* name, NE_Callback cb, void* ctx); // returns an id > 0, or 0
    typedef void (*NE_UnsubscribeFn)(int id);
    typedef void (*NE_PublishFn)(const NE_Event* e);
}

namespace ne
{
    struct ApiTable
    {
        NE_SubscribeFn Subscribe;
        NE_UnsubscribeFn Unsubscribe;
        NE_PublishFn Publish;
    };

    // Finds the loaded NorseEvents DLL beside the calling mod (...\ue4ss\Mods\NorseEvents\dlls\main.dll). Null when
    // NorseEvents is not installed or not loaded yet - call it again later (from on_unreal_init or a first use).
    inline const ApiTable* Api()
    {
        static ApiTable t{};
        if (t.Subscribe) return &t;
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&Api), &self);
        wchar_t buf[MAX_PATH * 2]{};
        GetModuleFileNameW(self, buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])));
        std::filesystem::path dll = std::filesystem::path(buf).parent_path().parent_path().parent_path() / L"NorseEvents" / L"dlls" / L"main.dll";
        HMODULE m = GetModuleHandleW(dll.c_str());
        if (!m) return nullptr;
        t.Subscribe = reinterpret_cast<NE_SubscribeFn>(GetProcAddress(m, "NE_Subscribe"));
        t.Unsubscribe = reinterpret_cast<NE_UnsubscribeFn>(GetProcAddress(m, "NE_Unsubscribe"));
        t.Publish = reinterpret_cast<NE_PublishFn>(GetProcAddress(m, "NE_Publish"));
        if (!t.Subscribe || !t.Unsubscribe || !t.Publish) t = {};
        return t.Subscribe ? &t : nullptr;
    }
} // namespace ne
