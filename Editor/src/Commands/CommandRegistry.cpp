#include "CommandRegistry.h"

#include <cmath>
#include <utility>

namespace SpiralEditor
{
    namespace
    {
        bool IsValidLabel(std::string_view text, size_t maximumBytes)
        {
            if (text.empty() || text.size() > maximumBytes)
                return false;
            for (const char c : text)
                if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F)
                    return false;
            return true;
        }

        CommandAvailability Evaluate(const CommandDescriptor& descriptor)
        {
            if (!descriptor.IsEnabled)
                return {};
            CommandAvailability availability = descriptor.IsEnabled();
            if (!availability.Enabled && availability.Reason.empty())
                availability.Reason = "Unavailable";
            return availability;
        }

        bool SourceAllowed(const CommandDescriptor& descriptor, CommandSource source)
        {
            return (descriptor.AllowedSources & CommandSourceBit(source)) != 0;
        }

        std::string_view ArgumentKindDescription(CommandArgumentKind kind)
        {
            switch (kind)
            {
            case CommandArgumentKind::None: return "no argument";
            case CommandArgumentKind::Flag: return "a flag";
            case CommandArgumentKind::Integer: return "an integer";
            case CommandArgumentKind::Real: return "a number";
            case CommandArgumentKind::Text: return "text";
            case CommandArgumentKind::Reals: return "a list of numbers";
            case CommandArgumentKind::Ids: return "a list of ids";
            }
            return "an unknown argument";
        }

        // Empty when the argument is acceptable, else the refusal shown to the caller.
        std::string ArgumentRefusal(CommandArgumentKind expected, const CommandArgument& argument)
        {
            if (ArgumentKindOf(argument) != expected)
                return expected == CommandArgumentKind::None ? "Takes no argument" : "Expects " + std::string(ArgumentKindDescription(expected));
            if (const auto* text = std::get_if<std::string>(&argument))
                return text->size() > CommandRegistry::kMaximumCommandArgumentBytes ? "Argument too long" : std::string();
            if (const auto* ids = std::get_if<std::vector<Engine::u64>>(&argument))
                return ids->size() > CommandRegistry::kMaximumCommandArgumentElements ? "Argument too long" : std::string();
            if (const auto* reals = std::get_if<std::vector<double>>(&argument))
            {
                if (reals->size() > CommandRegistry::kMaximumCommandArgumentElements)
                    return "Argument too long";
                for (const double value : *reals)
                    if (!std::isfinite(value))
                        return "Argument must be finite";
            }
            else if (const auto* real = std::get_if<double>(&argument); real && !std::isfinite(*real))
                return "Argument must be finite";
            return {};
        }

        std::string SourceRefusal(CommandSource source)
        {
            return "Not available from " + std::string(CommandSourceName(source));
        }
    }

    std::string_view CommandSourceName(CommandSource source)
    {
        switch (source)
        {
        case CommandSource::Menu: return "menu";
        case CommandSource::Shortcut: return "shortcut";
        case CommandSource::Palette: return "palette";
        case CommandSource::Typed: return "typed control";
        }
        return "unknown";
    }

    RegisterResult CommandRegistry::Register(CommandDescriptor descriptor)
    {
        if (!IsValidEditorIdentifier(descriptor.Id))
            return { RegisterStatus::InvalidId, {} };
        if (!IsValidLabel(descriptor.Title, kMaximumTitleBytes))
            return { RegisterStatus::InvalidTitle, {} };
        if (!IsValidLabel(descriptor.Category, kMaximumTitleBytes))
            return { RegisterStatus::InvalidCategory, {} };
        if (!descriptor.Execute)
            return { RegisterStatus::MissingExecute, {} };
        if (descriptor.AllowedSources == 0 || (descriptor.AllowedSources & ~kAllCommandSources) != 0)
            return { RegisterStatus::InvalidSources, {} };
        if (static_cast<Engine::u8>(descriptor.ArgumentKind) > static_cast<Engine::u8>(CommandArgumentKind::Ids))
            return { RegisterStatus::InvalidArgumentKind, {} };
        if (m_Index.contains(descriptor.Id))
            return { RegisterStatus::DuplicateId, {} };
        if (m_Commands.size() >= kMaximumCommands)
            return { RegisterStatus::LimitReached, {} };

        ShortcutMap defaults = m_DefaultShortcuts;
        for (const ShortcutKey& key : descriptor.DefaultShortcuts)
        {
            const BindResult bound = defaults.Bind(descriptor.Id, key);
            if (bound.Status == BindStatus::Conflict)
                return { RegisterStatus::ShortcutConflict, bound.ConflictingCommand };
            if (!bound.Ok())
                return { RegisterStatus::InvalidShortcut, {} };
        }

        m_Index.emplace(descriptor.Id, m_Commands.size());
        m_Commands.push_back(std::move(descriptor));
        m_DefaultShortcuts = std::move(defaults);
        return {};
    }

    const CommandDescriptor* CommandRegistry::Find(std::string_view id) const
    {
        const auto it = m_Index.find(std::string(id));
        return it == m_Index.end() ? nullptr : &m_Commands[it->second];
    }

    CommandAvailability CommandRegistry::Query(std::string_view id, CommandSource source) const
    {
        const CommandDescriptor* descriptor = Find(id);
        if (!descriptor)
            return CommandAvailability::Disabled("Unknown command");
        if (!SourceAllowed(*descriptor, source))
            return CommandAvailability::Disabled(SourceRefusal(source));
        return Evaluate(*descriptor);
    }

    DispatchResult CommandRegistry::Finish(DispatchResult result)
    {
        if (m_Observer && result.Status != DispatchStatus::TooDeep)
        {
            const Observer observer = m_Observer;
            ++m_Depth;
            observer(result);
            --m_Depth;
        }
        return result;
    }

    DispatchResult CommandRegistry::Dispatch(std::string_view id, CommandSource source, const CommandArgument& argument)
    {
        DispatchResult result;
        result.CommandId = std::string(id);
        result.Source = source;

        if (m_Depth >= kMaximumDispatchDepth)
        {
            result.Status = DispatchStatus::TooDeep;
            result.Reason = "Commands nested too deeply";
            return Finish(std::move(result));
        }
        const CommandDescriptor* descriptor = Find(id);
        if (!descriptor)
        {
            result.Status = DispatchStatus::NotFound;
            result.Reason = "Unknown command";
            return Finish(std::move(result));
        }
        if (!SourceAllowed(*descriptor, source))
        {
            result.Status = DispatchStatus::SourceNotAllowed;
            result.Reason = SourceRefusal(source);
            return Finish(std::move(result));
        }
        if (std::string refusal = ArgumentRefusal(descriptor->ArgumentKind, argument); !refusal.empty())
        {
            result.Status = DispatchStatus::InvalidArgument;
            result.Reason = std::move(refusal);
            return Finish(std::move(result));
        }
        if (const CommandAvailability availability = Evaluate(*descriptor); !availability.Enabled)
        {
            result.Status = DispatchStatus::Disabled;
            result.Reason = availability.Reason;
            return Finish(std::move(result));
        }

        // A command may register further commands; the vector can move, so run a copy.
        const auto execute = descriptor->Execute;
        ++m_Depth;
        CommandOutcome outcome = execute(CommandInvocation { source, argument });
        --m_Depth;
        if (outcome.Succeeded)
        {
            result.Status = DispatchStatus::Executed;
            result.Message = std::move(outcome.Message);
        }
        else
        {
            result.Status = DispatchStatus::Failed;
            result.Reason = outcome.Message.empty() ? "Command failed" : std::move(outcome.Message);
        }
        return Finish(std::move(result));
    }
}
