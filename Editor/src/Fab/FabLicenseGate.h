#pragma once

#include "Engine/Assets/FabImportReceipt.h"
#include "FabImportController.h"

#include <array>
#include <string>
#include <string_view>

// The licence-kind gate applied when a provenance declaration is confirmed. It is
// pure: no ImGui, filesystem, or global state. Spiral never chooses a licence for
// the user; the gate only decides whether the licence the user declared can be
// used as importable source content in Spiral.
//
// Source claims (Fab EULA of 2024-10-01 and Fab licences documentation, read
// 2026-10-10; research, not legal advice): the Fab Standard licence is not tied to
// Unreal Engine; Personal Reference Only listings give a reference snapshot and no
// source; code plugins are per-seat engine plugins; legacy Unreal Marketplace and
// Epic-owned content can be licensed for Unreal Engine use only; CC-BY content is
// governed by its own licence and requires attribution. The receipt schema
// (version 1) has no Other family and no NoAI acknowledgement field and is not
// changed here, so an Other licence is refused rather than recorded untruthfully.
namespace Fab
{
    enum class FabLicenseChoice
    {
        NotChosen,
        StandardPersonal,
        StandardProfessional,
        CcBy,
        PersonalReferenceOnly,
        CodePlugin,
        // Legacy Unreal Marketplace and Epic-owned content that may be UE-only.
        LegacyUeOnly,
        Other
    };

    // The order the Fab Import panel lists them in (NotChosen first).
    inline constexpr std::array<FabLicenseChoice, 8> kFabLicenseChoices { {
        FabLicenseChoice::NotChosen,
        FabLicenseChoice::StandardPersonal,
        FabLicenseChoice::StandardProfessional,
        FabLicenseChoice::CcBy,
        FabLicenseChoice::PersonalReferenceOnly,
        FabLicenseChoice::CodePlugin,
        FabLicenseChoice::LegacyUeOnly,
        FabLicenseChoice::Other,
    } };

    std::string_view FabLicenseChoiceLabel(FabLicenseChoice choice);
    // True for the choices the receipt schema cannot represent and the controller
    // therefore never holds (they exist only in the panel's form).
    bool IsPanelOnlyLicenseChoice(FabLicenseChoice choice);

    enum class FabLicenseVerdict
    {
        // Nothing usable has been chosen yet, or a required field is missing.
        Incomplete,
        Allowed,
        Refused
    };

    struct FabLicenseGateResult
    {
        FabLicenseVerdict Verdict = FabLicenseVerdict::Incomplete;
        // Plain explanation for Incomplete and Refused; empty for Allowed.
        std::string Message;
        // The listing is declared NoAI: the panel shows the acknowledgement checkbox.
        // This never blocks anything.
        bool NoAiNoticeRequired = false;
    };

    // `attributionText` is the user's attribution text and `noAi` the declared NoAI
    // flag. Reference-Only, code-plugin, UE-only and Other choices are refused;
    // CC-BY requires non-blank attribution text; Standard Personal and Professional
    // are allowed.
    FabLicenseGateResult EvaluateFabLicenseGate(FabLicenseChoice choice, std::string_view attributionText, Engine::FabMetadataFlag noAi);

    // The receipt licence fields a choice stands for. Choices the receipt cannot
    // represent (NotChosen, CodePlugin, Other) map to Unknown/Unknown.
    struct FabLicenseFields
    {
        Engine::FabLicenseFamily Family = Engine::FabLicenseFamily::Unknown;
        Engine::FabLicenseTier Tier = Engine::FabLicenseTier::Unknown;
    };
    FabLicenseFields FabLicenseFieldsForChoice(FabLicenseChoice choice);

    // The choice a controller-held provenance stands for. A Fab Standard family
    // without a Personal or Professional tier is NotChosen.
    FabLicenseChoice FabLicenseChoiceFromProvenance(const FabProvenance& provenance);

    // Convenience: the gate applied to a provenance exactly as the controller holds it.
    FabLicenseGateResult EvaluateFabLicenseGate(const FabProvenance& provenance);
}
