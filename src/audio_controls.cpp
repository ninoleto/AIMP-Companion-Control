#include "audio_controls.h"
#include "audio_catalog.h"
#include "globals.h"
#include "utils.h"
#include <utility>

namespace {
using namespace audio_control;

template<class T> class Ref {
    T* value_ = nullptr;
public:
    Ref() = default;
    Ref(const Ref&) = delete;
    Ref& operator=(const Ref&) = delete;
    ~Ref() { if (value_) value_->Release(); }
    T* get() const { return value_; }
    T* operator->() const { return value_; }
    explicit operator bool() const { return value_ != nullptr; }
    T** out() { if (value_) value_->Release(); value_ = nullptr; return &value_; }
    void** outVoid() { return reinterpret_cast<void**>(out()); }
};

bool OnMainThread() { return g_core && GetCurrentThreadId() == g_mainThreadId; }

template<class T> bool Service(REFIID iid, Ref<T>& service) {
    return g_core && g_core->QueryInterface(iid, service.outVoid()) == S_OK && service;
}

json Failure(const char* code, HRESULT result = S_OK, const std::string& detail = {}) {
    json error{{"code", code}};
    if (result != S_OK) error["hresult"] = static_cast<std::int32_t>(result);
    if (!detail.empty()) error["detail"] = detail;
    return {{"error", error}};
}

HRESULT ReadParameter(IAIMPServiceMessageDispatcher* messages, const Parameter& parameter, json& value) {
    if (!messages) return E_NOINTERFACE;
    const int property = AIMP_MSG_PROPERTY_BASE + parameter.messageOffset;
    if (parameter.boolean) {
        BOOL actual = FALSE;
        const HRESULT result = messages->Send(property, AIMP_MSG_PROPVALUE_GET, &actual);
        if (result == S_OK) value = actual != FALSE;
        return result;
    }
    float actual = 0;
    HRESULT result = messages->Send(property, AIMP_MSG_PROPVALUE_GET, &actual);
    if (result == S_OK && !std::isfinite(actual)) result = E_UNEXPECTED;
    if (result == S_OK) value = DisplayNumber(actual);
    return result;
}

HRESULT WriteParameter(IAIMPServiceMessageDispatcher* messages, const Parameter& parameter, const json& value) {
    const int property = AIMP_MSG_PROPERTY_BASE + parameter.messageOffset;
    if (parameter.boolean) {
        BOOL target = value.get<bool>() ? TRUE : FALSE;
        return messages->Send(property, AIMP_MSG_PROPVALUE_SET, &target);
    }
    float target = value.get<float>();
    return messages->Send(property, AIMP_MSG_PROPVALUE_SET, &target);
}

json PresetNames() {
    Ref<IAIMPServicePlayerEqualizerPresets> presets;
    if (!Service(IID_IAIMPServicePlayerEqualizerPresets, presets)) return json::array();
    const int count = presets->GetPresetCount();
    if (count < 0 || count > MaxPresets) return json::array();
    json names = json::array();
    for (int index = 0; index < count; ++index) {
        Ref<IAIMPEqualizerPreset> preset;
        if (presets->GetPreset(index, IID_IAIMPEqualizerPreset, preset.outVoid()) != S_OK || !preset) continue;
        Ref<IAIMPString> name;
        if (preset->GetName(name.out()) == S_OK && name) {
            std::string text = WStr(name->GetData());
            if (!text.empty() && text.size() <= 512) names.push_back(std::move(text));
        }
    }
    return names;
}

HRESULT ReadEqualizerEnabled(BOOL& enabled) {
    // Do not call IAIMPServicePlayerEqualizer::GetActive. The official Delphi
    // SDK declares that method with its default register convention, whereas
    // the C++ header declares WINAPI/stdcall. Those disagree on Win32 stack
    // cleanup. The documented message property uses the stable dispatcher ABI.
    Ref<IAIMPServiceMessageDispatcher> messages;
    if (!Service(IID_IAIMPServiceMessageDispatcher, messages)) return E_NOINTERFACE;
    enabled = FALSE;
    return messages->Send(AIMP_MSG_PROPERTY_EQUALIZER, AIMP_MSG_PROPVALUE_GET, &enabled);
}

json EqualizerState() {
    Ref<IAIMPServicePlayerEqualizer> equalizer;
    json state{{"available", false}, {"bands", json::array()}, {"presets", json::array()}};
    if (!Service(IID_IAIMPServicePlayerEqualizer, equalizer)) return state;
    state["available"] = true;
    BOOL enabled = FALSE;
    const HRESULT enabledResult = ReadEqualizerEnabled(enabled);
    if (enabledResult == S_OK) state["enabled"] = enabled != FALSE;
    else {
        state["enabled"] = nullptr;
        state["read_errors"]["enabled"] = static_cast<std::int32_t>(enabledResult);
    }
    double preamp = 0;
    const HRESULT preampResult = equalizer->GetPreamp(&preamp);
    if (preampResult == S_OK && std::isfinite(preamp) && preamp >= EqMinimum && preamp <= EqMaximum) state["preamp"] = DisplayNumber(preamp);
    else state["read_errors"]["preamp"] = preampResult == S_OK ? E_UNEXPECTED : preampResult;
    const int count = equalizer->GetBandCount();
    if (count < 0 || count > MaxBands) state["read_errors"]["bands"] = "INVALID_BAND_COUNT";
    else for (int index = 0; index < count; ++index) {
        double frequency = 0, gain = 0;
        HRESULT result = equalizer->GetBandFrequency(index, &frequency);
        if (result == S_OK) result = equalizer->GetBandGain(index, &gain);
        if (result != S_OK || !std::isfinite(frequency) || !std::isfinite(gain) || frequency <= 0 || gain < EqMinimum || gain > EqMaximum) {
            state["read_errors"]["band_" + std::to_string(index)] = result == S_OK ? E_UNEXPECTED : result;
            continue;
        }
        state["bands"].push_back({{"index", index}, {"frequency", DisplayNumber(frequency)}, {"gain", DisplayNumber(gain)}});
    }
    state["presets"] = PresetNames();
    state["preset"] = nullptr;
    Ref<IAIMPEqualizerPreset> selected;
    if (equalizer->GetPreset(IID_IAIMPEqualizerPreset, selected.outVoid()) == S_OK && selected) {
        Ref<IAIMPString> name;
        if (selected->GetName(name.out()) == S_OK && name) state["preset"] = WStr(name->GetData());
    }
    return state;
}

json Snapshot() {
    json state{{"audio", json::object()}};
    Ref<IAIMPServiceMessageDispatcher> messages;
    if (Service(IID_IAIMPServiceMessageDispatcher, messages)) {
        for (const auto& parameter : Parameters) {
            json value;
            if (ReadParameter(messages.get(), parameter, value) == S_OK) state["audio"][parameter.id] = value;
        }
    }
    state["equalizer"] = EqualizerState();
    return state;
}

void CompleteMutation(json& response, HRESULT result, const char* code, const std::string& detail = {}) {
    response = Snapshot();
    if (result != S_OK) response.update(Failure(code, result, detail));
}

json SaveCurrentBands(IAIMPServicePlayerEqualizer* equalizer, const json& command) {
    if (!command.contains("name") || !command["name"].is_string() ||
        !SafePresetName(command["name"].get_ref<const std::string&>())) return Failure("INVALID_EQUALIZER_PRESET_NAME");
    const std::string wanted = command["name"].get<std::string>();
    std::wstring wide = Utf8ToWStr(wanted);
    if (wide.empty() || wide.size() > 128) return Failure("INVALID_EQUALIZER_PRESET_NAME");
    Ref<IAIMPServicePlayerEqualizerPresets> presets;
    if (!Service(IID_IAIMPServicePlayerEqualizerPresets, presets)) return Failure("EQUALIZER_PRESETS_UNAVAILABLE");
    Ref<IAIMPString> name;
    HRESULT result = g_core->CreateObject(IID_IAIMPString, name.outVoid());
    if (result == S_OK && name) result = name->SetData(wide.data(), static_cast<int>(wide.size()));
    if (result != S_OK || !name) return Failure("EQUALIZER_PRESET_SAVE_FAILED", result == S_OK ? E_UNEXPECTED : result);

    // Fail closed if the complete existing library cannot be checked. In
    // particular, never invoke Add on an existing name (case insensitive).
    const int count = presets->GetPresetCount();
    if (count < 0 || count >= MaxPresets) return Failure("INVALID_EQUALIZER_PRESET_COUNT");
    for (int index = 0; index < count; ++index) {
        Ref<IAIMPEqualizerPreset> existing;
        result = presets->GetPreset(index, IID_IAIMPEqualizerPreset, existing.outVoid());
        if (result != S_OK || !existing) return Failure("EQUALIZER_PRESET_ENUMERATION_FAILED", result == S_OK ? E_UNEXPECTED : result);
        Ref<IAIMPString> existingName;
        result = existing->GetName(existingName.out());
        if (result != S_OK || !existingName) return Failure("EQUALIZER_PRESET_ENUMERATION_FAILED", result == S_OK ? E_UNEXPECTED : result);
        int comparison = 0;
        result = existingName->Compare(name.get(), &comparison, TRUE);
        if (result != S_OK) return Failure("EQUALIZER_PRESET_ENUMERATION_FAILED", result);
        if (!comparison) return Failure("EQUALIZER_PRESET_ALREADY_EXISTS");
    }

    const int bands = equalizer->GetBandCount();
    if (bands <= 0 || bands > MaxBands) return Failure("INVALID_EQUALIZER_BAND_COUNT");
    std::vector<std::pair<double, double>> values;
    values.reserve(bands);
    for (int index = 0; index < bands; ++index) {
        double frequency = 0, gain = 0;
        result = equalizer->GetBandFrequency(index, &frequency);
        if (result == S_OK) result = equalizer->GetBandGain(index, &gain);
        if (result != S_OK || !std::isfinite(frequency) || frequency <= 0 || !std::isfinite(gain) || gain < EqMinimum || gain > EqMaximum)
            return Failure("EQUALIZER_STATE_UNAVAILABLE", result == S_OK ? E_UNEXPECTED : result);
        values.emplace_back(frequency, gain);
    }

    Ref<IAIMPEqualizerPreset> created;
    result = presets->Add(name.get(), IID_IAIMPEqualizerPreset, created.outVoid());
    if (result != S_OK || !created) return Failure("EQUALIZER_PRESET_SAVE_FAILED", result == S_OK ? E_UNEXPECTED : result);
    if (created->GetBandCount() != bands) result = E_UNEXPECTED;
    for (int index = 0; result == S_OK && index < bands; ++index) {
        double frequency = 0, gain = 0;
        result = created->GetBandFrequency(index, &frequency);
        if (result == S_OK && (!std::isfinite(frequency) || std::abs(frequency - values[index].first) > 0.0001)) result = E_UNEXPECTED;
        if (result == S_OK) result = created->SetBandGain(index, values[index].second);
        if (result == S_OK) result = created->GetBandGain(index, &gain);
        if (result == S_OK && (!std::isfinite(gain) || gain < EqMinimum || gain > EqMaximum)) result = E_UNEXPECTED;
    }
    if (result != S_OK) {
        // Only remove the exact newly created object if initialization failed.
        // This is separate from explicit, confirmed preset deletion below.
        HRESULT cleanup = presets->Delete(created.get());
        json response = Failure("EQUALIZER_PRESET_SAVE_FAILED", result);
        if (cleanup != S_OK) response["error"]["cleanup_hresult"] = static_cast<std::int32_t>(cleanup);
        return response;
    }
    json response = Snapshot();
    // IAIMPEqualizerPreset only exposes bands and name, not preamp or enabled.
    response["saved_preset"] = {{"name", wanted}, {"includes_preamp", false}, {"band_count", bands}};
    return response;
}

json ManagePreset(const json& command, bool rename) {
    if (!command.contains("confirm") || !command["confirm"].is_boolean() || !command["confirm"].get<bool>())
        return Failure("CONFIRMATION_REQUIRED");
    if (!command.contains("name") || !command["name"].is_string()) return Failure("INVALID_EQUALIZER_PRESET_NAME");
    const std::string wanted = command["name"].get<std::string>();
    if (wanted.empty() || wanted.size() > 512 || wanted.find('\0') != std::string::npos)
        return Failure("INVALID_EQUALIZER_PRESET_NAME");
    Ref<IAIMPString> newName;
    std::string replacement;
    HRESULT result = S_OK;
    if (rename) {
        if (!command.contains("new_name") || !command["new_name"].is_string() ||
            !SafePresetName(command["new_name"].get_ref<const std::string&>())) return Failure("INVALID_EQUALIZER_PRESET_NAME");
        replacement = command["new_name"].get<std::string>();
        std::wstring wide = Utf8ToWStr(replacement);
        if (wide.empty() || wide.size() > 128) return Failure("INVALID_EQUALIZER_PRESET_NAME");
        result = g_core->CreateObject(IID_IAIMPString, newName.outVoid());
        if (result == S_OK && newName) result = newName->SetData(wide.data(), static_cast<int>(wide.size()));
        if (result != S_OK || !newName) return Failure("EQUALIZER_PRESET_RENAME_FAILED", result == S_OK ? E_UNEXPECTED : result);
    }
    Ref<IAIMPServicePlayerEqualizerPresets> presets;
    if (!Service(IID_IAIMPServicePlayerEqualizerPresets, presets)) return Failure("EQUALIZER_PRESETS_UNAVAILABLE");
    const int beforeCount = presets->GetPresetCount();
    if (beforeCount < 0 || beforeCount > MaxPresets) return Failure("INVALID_EQUALIZER_PRESET_COUNT");
    Ref<IAIMPEqualizerPreset> selected;
    for (int index = 0; index < beforeCount; ++index) {
        Ref<IAIMPEqualizerPreset> item;
        Ref<IAIMPString> name;
        result = presets->GetPreset(index, IID_IAIMPEqualizerPreset, item.outVoid());
        if (result == S_OK && item) result = item->GetName(name.out());
        if (result != S_OK || !item || !name) return Failure("EQUALIZER_PRESET_ENUMERATION_FAILED", result == S_OK ? E_UNEXPECTED : result);
        const std::string actualName = WStr(name->GetData());
        if (actualName == wanted) {
            if (selected) return Failure("AMBIGUOUS_EQUALIZER_PRESET_NAME");
            item->AddRef();
            *selected.out() = item.get();
        } else if (rename) {
            int comparison = 0;
            result = name->Compare(newName.get(), &comparison, TRUE);
            if (result != S_OK) return Failure("EQUALIZER_PRESET_ENUMERATION_FAILED", result);
            if (comparison == 0) return Failure("EQUALIZER_PRESET_ALREADY_EXISTS");
        }
    }
    if (!selected) return Failure("EQUALIZER_PRESET_NOT_FOUND");
    if (rename && replacement == wanted) {
        json response = Snapshot();
        response["renamed_preset"] = {{"old_name", wanted}, {"name", replacement}, {"changed", false}};
        return response;
    }
    // The SDK exposes no built-in/user marker. The caller must explicitly
    // confirm the exact enumerated name; factory presets are not exempt.
    result = rename ? selected->SetName(newName.get()) : presets->Delete(selected.get());
    if (result != S_OK) {
        json response = Snapshot();
        response.update(Failure(rename ? "EQUALIZER_PRESET_RENAME_FAILED" : "EQUALIZER_PRESET_DELETE_FAILED", result));
        return response;
    }
    const int afterCount = presets->GetPresetCount();
    bool verified = afterCount == beforeCount - (rename ? 0 : 1);
    int destinationMatches = 0;
    if (afterCount < 0 || afterCount > MaxPresets) verified = false;
    else for (int index = 0; index < afterCount; ++index) {
        Ref<IAIMPEqualizerPreset> item;
        Ref<IAIMPString> name;
        HRESULT read = presets->GetPreset(index, IID_IAIMPEqualizerPreset, item.outVoid());
        if (read == S_OK && item) read = item->GetName(name.out());
        if (read != S_OK || !item || !name) { verified = false; break; }
        const std::string actualName = WStr(name->GetData());
        if (actualName == wanted) verified = false;
        if (rename && actualName == replacement) ++destinationMatches;
    }
    if (rename && destinationMatches != 1) verified = false;
    json response = Snapshot();
    response[rename ? "renamed_preset" : "deleted_preset"] = rename
        ? json{{"old_name", wanted}, {"name", replacement}, {"changed", true}}
        : json{{"name", wanted}};
    if (!verified) {
        response.update(Failure("EQUALIZER_PRESET_READBACK_FAILED"));
        response["mutation_applied"] = true;
    }
    return response;
}

bool KnownAction(const std::string& action) {
    static const std::set<std::string> actions{
        "audio/get", "audio/set", "audio/adjust", "audio/reset", "audio/toggle",
        "equalizer/get", "equalizer/set", "equalizer/toggle", "equalizer/reset",
        "equalizer/band/set", "equalizer/band/adjust", "equalizer/preamp/set", "equalizer/preamp/adjust",
        "equalizer/presets/get", "equalizer/preset/apply", "equalizer/preset/save",
        "equalizer/preset/rename", "equalizer/preset/delete"
    };
    return actions.count(action) != 0;
}
} // namespace

json GetAudioCapabilities() {
    if (!OnMainThread()) return Failure("MAIN_THREAD_REQUIRED");
    json result{{"audio", json::array()}};
    Ref<IAIMPServiceMessageDispatcher> messages;
    Service(IID_IAIMPServiceMessageDispatcher, messages);
    for (const auto& parameter : Parameters) {
        json value;
        json descriptor{{"id", parameter.id}, {"label", parameter.label}, {"min", parameter.minimum},
                        {"max", parameter.maximum}, {"default", parameter.neutral}, {"step", parameter.step},
                        {"unit", parameter.unit}, {"type", parameter.boolean ? "boolean" : "number"},
                        {"supported", ReadParameter(messages.get(), parameter, value) == S_OK}};
        if (parameter.boolean) descriptor["default"] = parameter.neutral != 0;
        result["audio"].push_back(std::move(descriptor));
    }
    Ref<IAIMPServicePlayerEqualizer> equalizer;
    const bool available = Service(IID_IAIMPServicePlayerEqualizer, equalizer);
    result["equalizer"] = {{"available", available}, {"band_count", available ? equalizer->GetBandCount() : 0},
        {"min", EqMinimum}, {"max", EqMaximum}, {"step", 0.5}, {"unit", "dB"},
        {"preamp", {{"min", EqMinimum}, {"max", EqMaximum}, {"default", 0}, {"step", 0.5}, {"unit", "dB"}}}};
    Ref<IAIMPServicePlayerEqualizerPresets> presets;
    const bool presetsAvailable = Service(IID_IAIMPServicePlayerEqualizerPresets, presets);
    result["equalizer"]["presets_available"] = presetsAvailable;
    result["equalizer"]["preset_save"] = available && presetsAvailable;
    result["equalizer"]["preset_save_preamp"] = false;
    result["equalizer"]["preset_rename"] = presetsAvailable;
    result["equalizer"]["preset_delete"] = presetsAvailable;
    result["equalizer"]["preset_builtin_distinction"] = false;
    result["equalizer"]["preset_management_requires_confirmation"] = true;
    return result;
}

json GetAudioState() {
    if (!OnMainThread()) return Failure("MAIN_THREAD_REQUIRED");
    return Snapshot();
}

bool HandleAudioCommand(const std::string& action, const json& command, json& response) {
    if (!KnownAction(action)) return false;
    if (!OnMainThread()) { response = Failure("MAIN_THREAD_REQUIRED"); return true; }
    if (!command.is_object()) { response = Failure("INVALID_COMMAND"); return true; }
    if (action == "audio/get" || action == "equalizer/get" || action == "equalizer/presets/get") {
        response = Snapshot();
        return true;
    }
    if (action.compare(0, 6, "audio/") == 0) {
        const bool all = action == "audio/reset" && !command.contains("parameter");
        const Parameter* parameter = nullptr;
        if (!all) {
            if (!command.contains("parameter") || !command["parameter"].is_string()) {
                response = Failure("INVALID_AUDIO_PARAMETER"); return true;
            }
            parameter = FindParameter(command["parameter"].get<std::string>());
            if (!parameter) { response = Failure("UNSUPPORTED_AUDIO_PARAMETER"); return true; }
        }
        Ref<IAIMPServiceMessageDispatcher> messages;
        if (!Service(IID_IAIMPServiceMessageDispatcher, messages)) {
            response = Failure("AUDIO_SERVICE_UNAVAILABLE"); return true;
        }
        if (all) {
            json failures = json::object();
            int supported = 0;
            for (const auto& candidate : Parameters) {
                json current;
                // Only reset parameters actually provided by this AIMP installation.
                if (ReadParameter(messages.get(), candidate, current) != S_OK) continue;
                ++supported;
                json target = candidate.boolean ? json(candidate.neutral != 0) : json(candidate.neutral);
                HRESULT result = WriteParameter(messages.get(), candidate, target);
                if (result == S_OK) result = ReadParameter(messages.get(), candidate, current);
                if (result != S_OK) failures[candidate.id] = static_cast<std::int32_t>(result);
            }
            response = Snapshot();
            if (!supported) response.update(Failure("AUDIO_PARAMETERS_UNAVAILABLE"));
            else if (!failures.empty()) response["error"] = {{"code", "AUDIO_RESET_PARTIALLY_FAILED"}, {"parameters", failures}};
            return true;
        }
        json current;
        HRESULT result = ReadParameter(messages.get(), *parameter, current);
        if (result != S_OK) { response = Failure("AUDIO_PARAMETER_UNAVAILABLE", result, parameter->id); return true; }
        json target;
        if (action == "audio/reset") target = parameter->boolean ? json(parameter->neutral != 0) : json(parameter->neutral);
        else if (parameter->boolean) {
            if (action == "audio/toggle") target = !current.get<bool>();
            else if (action == "audio/set" && command.contains("value") && command["value"].is_boolean()) target = command["value"];
            else { response = Failure("INVALID_BOOLEAN_VALUE"); return true; }
        } else {
            double number = 0;
            if (action == "audio/toggle" || !NumberTarget(command, action == "audio/adjust", current.get<double>(), parameter->minimum, parameter->maximum, number)) {
                response = Failure("INVALID_AUDIO_VALUE", S_OK, parameter->id); return true;
            }
            target = number;
        }
        result = WriteParameter(messages.get(), *parameter, target);
        json actual;
        if (result == S_OK) result = ReadParameter(messages.get(), *parameter, actual);
        CompleteMutation(response, result, "AUDIO_OPERATION_FAILED", parameter->id);
        if (result == S_OK) response["audio"][parameter->id] = actual;
        return true;
    }

    if (action == "equalizer/preset/rename" || action == "equalizer/preset/delete") {
        response = ManagePreset(command, action == "equalizer/preset/rename");
        return true;
    }
    Ref<IAIMPServicePlayerEqualizer> equalizer;
    if (!Service(IID_IAIMPServicePlayerEqualizer, equalizer)) {
        response = Failure("EQUALIZER_UNAVAILABLE"); return true;
    }
    HRESULT result = S_OK;
    if (action == "equalizer/preset/save") {
        response = SaveCurrentBands(equalizer.get(), command);
        return true;
    } else if (action == "equalizer/toggle") {
        BOOL enabled = FALSE;
        result = ReadEqualizerEnabled(enabled);
        if (result == S_OK) result = equalizer->SetActive(enabled == FALSE ? TRUE : FALSE);
    } else if (action == "equalizer/set") {
        double preamp = 0;
        if ((!command.contains("enabled") && !command.contains("preamp")) ||
            (command.contains("enabled") && !command["enabled"].is_boolean()) ||
            (command.contains("preamp") && (!FiniteNumber(command, "preamp", preamp) || preamp < EqMinimum || preamp > EqMaximum))) {
            response = Failure("INVALID_EQUALIZER_VALUE"); return true;
        }
        // Validate every field before modifying either setting.
        if (command.contains("preamp")) result = equalizer->SetPreamp(preamp);
        if (result == S_OK && command.contains("enabled")) result = equalizer->SetActive(command["enabled"].get<bool>() ? TRUE : FALSE);
    } else if (action == "equalizer/preamp/set" || action == "equalizer/preamp/adjust") {
        double current = 0, target = 0;
        result = equalizer->GetPreamp(&current);
        if (result == S_OK && !std::isfinite(current)) result = E_UNEXPECTED;
        if (result == S_OK) {
            if (!NumberTarget(command, action == "equalizer/preamp/adjust", current, EqMinimum, EqMaximum, target)) {
                response = Failure("INVALID_EQUALIZER_VALUE"); return true;
            }
            result = equalizer->SetPreamp(target);
        }
        if (result == S_OK) result = equalizer->GetPreamp(&current);
        if (result == S_OK && (!std::isfinite(current) || current < EqMinimum || current > EqMaximum)) result = E_UNEXPECTED;
    } else if (action == "equalizer/band/set" || action == "equalizer/band/adjust") {
        const int count = equalizer->GetBandCount();
        int index = 0;
        if (!BandIndex(command, count, index)) { response = Failure("INVALID_EQUALIZER_BAND"); return true; }
        double current = 0, target = 0;
        result = equalizer->GetBandGain(index, &current);
        if (result == S_OK && !std::isfinite(current)) result = E_UNEXPECTED;
        if (result == S_OK) {
            if (!NumberTarget(command, action == "equalizer/band/adjust", current, EqMinimum, EqMaximum, target)) {
                response = Failure("INVALID_EQUALIZER_VALUE"); return true;
            }
            result = equalizer->SetBandGain(index, target);
        }
        if (result == S_OK) result = equalizer->GetBandGain(index, &current);
        if (result == S_OK && (!std::isfinite(current) || current < EqMinimum || current > EqMaximum)) result = E_UNEXPECTED;
    } else if (action == "equalizer/reset") {
        const int count = equalizer->GetBandCount();
        if (count < 0 || count > MaxBands) { response = Failure("INVALID_EQUALIZER_BAND_COUNT"); return true; }
        result = equalizer->SetPreamp(0);
        for (int index = 0; result == S_OK && index < count; ++index) result = equalizer->SetBandGain(index, 0);
        // Flattening is separate from enabling/disabling the processor.
    } else if (action == "equalizer/preset/apply") {
        if (!command.contains("name") || !command["name"].is_string() ||
            command["name"].get_ref<const std::string&>().empty() || command["name"].get_ref<const std::string&>().size() > 512) {
            response = Failure("INVALID_EQUALIZER_PRESET"); return true;
        }
        Ref<IAIMPServicePlayerEqualizerPresets> presets;
        if (!Service(IID_IAIMPServicePlayerEqualizerPresets, presets)) {
            response = Failure("EQUALIZER_PRESETS_UNAVAILABLE"); return true;
        }
        // Use exact names from enumeration. No filename is accepted or opened.
        const int count = presets->GetPresetCount();
        if (count < 0 || count > MaxPresets) { response = Failure("INVALID_EQUALIZER_PRESET_COUNT"); return true; }
        Ref<IAIMPEqualizerPreset> selected;
        const std::string wanted = command["name"].get<std::string>();
        bool found = false;
        for (int index = 0; index < count; ++index) {
            if (presets->GetPreset(index, IID_IAIMPEqualizerPreset, selected.outVoid()) != S_OK || !selected) continue;
            Ref<IAIMPString> name;
            if (selected->GetName(name.out()) == S_OK && name && WStr(name->GetData()) == wanted) { found = true; break; }
        }
        if (!found) { response = Failure("EQUALIZER_PRESET_NOT_FOUND"); return true; }
        result = equalizer->SetPreset(selected.get());
    }
    CompleteMutation(response, result, "EQUALIZER_OPERATION_FAILED");
    return true;
}
