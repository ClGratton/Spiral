#pragma once

#include "ShortcutMap.h"

#include "Engine/Core/Base.h"

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace SpiralEditor
{
    // Where a dispatch came from. Every surface calls the same Dispatch; the source only gates
    // which commands a surface may reach and is reported to the observer.
    enum class CommandSource : Engine::u8
    {
        Menu,
        Shortcut,
        Palette,
        Typed
    };

    constexpr Engine::u8 CommandSourceBit(CommandSource source)
    {
        return static_cast<Engine::u8>(1u << static_cast<Engine::u8>(source));
    }

    // Typed control must opt in per command: the registry never becomes arbitrary typed dispatch.
    inline constexpr Engine::u8 kDefaultCommandSources = CommandSourceBit(CommandSource::Menu)
        | CommandSourceBit(CommandSource::Shortcut) | CommandSourceBit(CommandSource::Palette);
    inline constexpr Engine::u8 kAllCommandSources = kDefaultCommandSources | CommandSourceBit(CommandSource::Typed);

    std::string_view CommandSourceName(CommandSource source);

    struct CommandAvailability
    {
        bool Enabled = true;
        std::string Reason; // shown for a disabled command; never empty when Enabled is false

        static CommandAvailability Disabled(std::string reason) { return { false, std::move(reason) }; }
    };

    struct CommandOutcome
    {
        bool Succeeded = true;
        std::string Message; // announcement on success, failure reason otherwise
    };

    // The closed set of argument shapes a command may declare; the variant index equals the enumerator.
    // There is no string-to-code dispatch: a typed caller supplies a value of exactly the declared shape.
    enum class CommandArgumentKind : Engine::u8
    {
        None,
        Flag,
        Integer,
        Real,
        Text,    // for example a panel id
        Reals,   // for example a transform or a color
        Ids      // for example stable entity ids
    };

    using CommandArgument = std::variant<std::monostate, bool, Engine::i64, double, std::string, std::vector<double>,
        std::vector<Engine::u64>>;

    constexpr CommandArgumentKind ArgumentKindOf(const CommandArgument& argument)
    {
        return static_cast<CommandArgumentKind>(argument.index());
    }

    struct CommandInvocation
    {
        CommandSource Source = CommandSource::Menu;
        // Already validated against the descriptor's ArgumentKind: the right shape, bounded, every real
        // finite. Further semantic validation (does this panel id exist?) stays with the command.
        const CommandArgument& Argument;
    };

    struct CommandDescriptor
    {
        std::string Id;       // IsValidEditorIdentifier grammar, for example "edit.undo"
        std::string Title;    // palette and menu label, for example "Undo"
        std::string Category; // grouping label, for example "Edit"
        std::vector<ShortcutKey> DefaultShortcuts;
        Engine::u8 AllowedSources = kDefaultCommandSources;
        // Dispatch refuses any other shape. A command that takes an argument is not reachable from a surface
        // that cannot supply one (the palette), which receives InvalidArgument and the expected shape.
        CommandArgumentKind ArgumentKind = CommandArgumentKind::None;
        // Null means always enabled. Must be cheap and side-effect free: menus call it every frame.
        std::function<CommandAvailability()> IsEnabled;
        // Required. Must not throw.
        std::function<CommandOutcome(const CommandInvocation&)> Execute;
    };

    enum class RegisterStatus : Engine::u8
    {
        Registered,
        InvalidId,
        InvalidTitle,
        InvalidCategory,
        MissingExecute,
        InvalidSources,
        InvalidArgumentKind,
        DuplicateId,
        InvalidShortcut,
        ShortcutConflict,
        LimitReached
    };

    struct RegisterResult
    {
        RegisterStatus Status = RegisterStatus::Registered;
        std::string ConflictingCommand; // set for ShortcutConflict

        bool Ok() const { return Status == RegisterStatus::Registered; }
    };

    enum class DispatchStatus : Engine::u8
    {
        Executed,
        Failed,           // the command ran and reported failure
        Disabled,         // its enabled predicate refused; Reason says why
        SourceNotAllowed, // the descriptor does not admit this source
        NotFound,
        InvalidArgument,  // wrong shape, over a bound, or a non-finite real; Reason says which
        TooDeep // nested dispatch beyond kMaximumDispatchDepth
    };

    struct DispatchResult
    {
        DispatchStatus Status = DispatchStatus::NotFound;
        std::string CommandId;
        CommandSource Source = CommandSource::Menu;
        std::string Reason;  // why nothing (or a failure) happened; empty when Executed
        std::string Message; // the command's announcement when Executed

        bool Executed() const { return Status == DispatchStatus::Executed; }
    };

    // The single action entry for menus, shortcuts, the palette, and typed automation. Commands are
    // enumerated in registration order. Main thread only. Descriptors are validated and registered
    // atomically; the default shortcut map is conflict-free by construction.
    class CommandRegistry
    {
    public:
        static constexpr size_t kMaximumCommands = 2048;
        static constexpr size_t kMaximumCommandArgumentBytes = 1024;
        static constexpr size_t kMaximumCommandArgumentElements = 4096;
        static constexpr size_t kMaximumTitleBytes = 128;
        static constexpr size_t kMaximumDispatchDepth = 8;

        using Observer = std::function<void(const DispatchResult&)>;

        RegisterResult Register(CommandDescriptor descriptor);

        // Registration order. References and pointers into this vector die on the next Register.
        const std::vector<CommandDescriptor>& Commands() const { return m_Commands; }
        const CommandDescriptor* Find(std::string_view id) const;

        // Whether Dispatch(id, source) would run the command right now, with the reason when not.
        CommandAvailability Query(std::string_view id, CommandSource source = CommandSource::Menu) const;

        DispatchResult Dispatch(std::string_view id, CommandSource source, const CommandArgument& argument = {});

        // Called once per Dispatch, after it finished, for every status except TooDeep (an observer that
        // dispatches would otherwise recurse without bound). Pass an empty function to clear.
        void SetObserver(Observer observer) { m_Observer = std::move(observer); }

        // Union of every descriptor's DefaultShortcuts.
        const ShortcutMap& DefaultShortcuts() const { return m_DefaultShortcuts; }

    private:
        DispatchResult Finish(DispatchResult result);

        std::vector<CommandDescriptor> m_Commands;
        std::unordered_map<std::string, size_t> m_Index;
        ShortcutMap m_DefaultShortcuts;
        Observer m_Observer;
        size_t m_Depth = 0;
    };
}
