#include "FabLicenseGate.h"

#include <algorithm>

namespace Fab
{
    namespace
    {
        bool HasVisibleText(std::string_view text)
        {
            return std::any_of(text.begin(), text.end(), [](char character)
            {
                return character != ' ' && character != '\t' && character != '\n' && character != '\r';
            });
        }

        constexpr std::string_view kUnusable = "cannot be used as importable source content in Spiral";
    }

    std::string_view FabLicenseChoiceLabel(FabLicenseChoice choice)
    {
        switch (choice)
        {
        case FabLicenseChoice::NotChosen: return "Not chosen";
        case FabLicenseChoice::StandardPersonal: return "Fab Standard - Personal";
        case FabLicenseChoice::StandardProfessional: return "Fab Standard - Professional";
        case FabLicenseChoice::CcBy: return "CC-BY (attribution required)";
        case FabLicenseChoice::PersonalReferenceOnly: return "Personal - Reference Only";
        case FabLicenseChoice::CodePlugin: return "Code plugin";
        case FabLicenseChoice::LegacyUeOnly: return "Legacy UE Marketplace / Epic-owned (UE only)";
        case FabLicenseChoice::Other: return "Other";
        }
        return "Not chosen";
    }

    bool IsPanelOnlyLicenseChoice(FabLicenseChoice choice)
    {
        return choice == FabLicenseChoice::CodePlugin || choice == FabLicenseChoice::Other;
    }

    FabLicenseGateResult EvaluateFabLicenseGate(FabLicenseChoice choice, std::string_view attributionText, Engine::FabMetadataFlag noAi)
    {
        FabLicenseGateResult result;
        result.NoAiNoticeRequired = noAi == Engine::FabMetadataFlag::Yes;
        switch (choice)
        {
        case FabLicenseChoice::NotChosen:
            result.Verdict = FabLicenseVerdict::Incomplete;
            result.Message = "Choose the licence shown on the Fab listing.";
            break;
        case FabLicenseChoice::StandardPersonal:
        case FabLicenseChoice::StandardProfessional:
            result.Verdict = FabLicenseVerdict::Allowed;
            break;
        case FabLicenseChoice::CcBy:
            if (HasVisibleText(attributionText))
            {
                result.Verdict = FabLicenseVerdict::Allowed;
            }
            else
            {
                result.Verdict = FabLicenseVerdict::Incomplete;
                result.Message = "CC-BY requires attribution: enter the attribution text from the listing.";
            }
            break;
        case FabLicenseChoice::PersonalReferenceOnly:
            result.Verdict = FabLicenseVerdict::Refused;
            result.Message = "Personal Reference Only listings provide a reference snapshot instead of source files, so this content "
                + std::string(kUnusable) + ".";
            break;
        case FabLicenseChoice::CodePlugin:
            result.Verdict = FabLicenseVerdict::Refused;
            result.Message = "Code plugins are per-seat Unreal or Unity engine plugins, so this content " + std::string(kUnusable) + ".";
            break;
        case FabLicenseChoice::LegacyUeOnly:
            result.Verdict = FabLicenseVerdict::Refused;
            result.Message = "Legacy Unreal Marketplace and Epic-owned content can be licensed for Unreal Engine use only, so this content "
                + std::string(kUnusable) + ". Use a listing offered under the Fab Standard or CC-BY licence.";
            break;
        case FabLicenseChoice::Other:
            result.Verdict = FabLicenseVerdict::Refused;
            result.Message = "Spiral cannot judge other licences and the import receipt cannot record them yet, so this content "
                + std::string(kUnusable) + ". Use a listing offered under the Fab Standard or CC-BY licence.";
            break;
        }
        return result;
    }

    FabLicenseFields FabLicenseFieldsForChoice(FabLicenseChoice choice)
    {
        switch (choice)
        {
        case FabLicenseChoice::StandardPersonal:
            return { Engine::FabLicenseFamily::FabStandard, Engine::FabLicenseTier::Personal };
        case FabLicenseChoice::StandardProfessional:
            return { Engine::FabLicenseFamily::FabStandard, Engine::FabLicenseTier::Professional };
        case FabLicenseChoice::CcBy:
            return { Engine::FabLicenseFamily::CreativeCommonsAttribution, Engine::FabLicenseTier::NotApplicable };
        case FabLicenseChoice::PersonalReferenceOnly:
            return { Engine::FabLicenseFamily::ReferenceOnly, Engine::FabLicenseTier::NotApplicable };
        case FabLicenseChoice::LegacyUeOnly:
            return { Engine::FabLicenseFamily::LegacyUnrealMarketplace, Engine::FabLicenseTier::NotApplicable };
        case FabLicenseChoice::NotChosen:
        case FabLicenseChoice::CodePlugin:
        case FabLicenseChoice::Other:
            break;
        }
        return {};
    }

    FabLicenseChoice FabLicenseChoiceFromProvenance(const FabProvenance& provenance)
    {
        switch (provenance.LicenseFamily)
        {
        case Engine::FabLicenseFamily::FabStandard:
            if (provenance.LicenseTier == Engine::FabLicenseTier::Personal)
                return FabLicenseChoice::StandardPersonal;
            if (provenance.LicenseTier == Engine::FabLicenseTier::Professional)
                return FabLicenseChoice::StandardProfessional;
            return FabLicenseChoice::NotChosen;
        case Engine::FabLicenseFamily::CreativeCommonsAttribution: return FabLicenseChoice::CcBy;
        case Engine::FabLicenseFamily::LegacyUnrealMarketplace: return FabLicenseChoice::LegacyUeOnly;
        case Engine::FabLicenseFamily::ReferenceOnly: return FabLicenseChoice::PersonalReferenceOnly;
        case Engine::FabLicenseFamily::Unknown: break;
        }
        return FabLicenseChoice::NotChosen;
    }

    FabLicenseGateResult EvaluateFabLicenseGate(const FabProvenance& provenance)
    {
        return EvaluateFabLicenseGate(FabLicenseChoiceFromProvenance(provenance), provenance.AttributionText, provenance.NoAI);
    }
}
