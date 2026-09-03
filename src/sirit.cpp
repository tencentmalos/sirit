/* This file is part of the sirit project.
 * Copyright (c) 2019 sirit
 * This software may be used and distributed according to the terms of the
 * 3-Clause BSD License
 */

#include <algorithm>
#include <array>
#include <cassert>
#include <stdexcept>

#include "sirit/sirit.h"

#include "common_types.h"
#include "stream.h"

namespace Sirit {

constexpr u32 MakeWord0(spv::Op op, size_t word_count) {
    return static_cast<u32>(op) | static_cast<u32>(word_count) << 16;
}

Module::Module(u32 version_)
    : version{version_}, ext_inst_imports{std::make_unique<Stream>(&bound)},
      entry_points{std::make_unique<Stream>(&bound)},
      execution_modes{std::make_unique<Stream>(&bound)}, debug{std::make_unique<Stream>(&bound)},
      annotations{std::make_unique<Stream>(&bound)}, declarations{std::make_unique<Declarations>(
                                                         &bound)},
      global_variables{std::make_unique<Stream>(&bound)}, code{std::make_unique<Stream>(&bound)} {}

Module::~Module() = default;

std::vector<u32> Module::Assemble() const {
    size_t total_words = 5 + capabilities.size() * 2;
    for (const std::string_view extension_name : extensions) {
        total_words += 1 + WordsInString(extension_name);
    }
    total_words += ext_inst_imports->Words().size() + 3 + entry_points->Words().size() +
                   execution_modes->Words().size() + debug->Words().size() +
                   annotations->Words().size() + declarations->Words().size() +
                   global_variables->Words().size() + code->Words().size();
    std::vector<u32> words(total_words);
    size_t insert_index = 0;
    const auto insert = [&words, &insert_index](std::span<const u32> input) {
        if (insert_index > words.size() || input.size() > words.size() - insert_index) {
            throw std::logic_error{"Sirit SPIR-V assembly size mismatch"};
        }
        const auto output = words.begin() +
                            static_cast<std::vector<u32>::difference_type>(insert_index);
        std::copy(input.begin(), input.end(), output);
        insert_index += input.size();
    };

    insert(std::array{
        spv::MagicNumber,
        version,
        GENERATOR_MAGIC_NUMBER,
        bound + 1,
        0u,
    });
    for (const spv::Capability capability : capabilities) {
        insert(std::array{
            MakeWord0(spv::Op::OpCapability, 2),
            static_cast<u32>(capability),
        });
    }

    for (const std::string_view extension_name : extensions) {
        const size_t string_words = WordsInString(extension_name);
        insert(std::array{MakeWord0(spv::Op::OpExtension, string_words + 1)});
        InsertStringView(words, insert_index, extension_name);
    }

    insert(ext_inst_imports->Words());

    insert(std::array{
        MakeWord0(spv::Op::OpMemoryModel, 3),
        static_cast<u32>(addressing_model),
        static_cast<u32>(memory_model),
    });

    insert(entry_points->Words());
    insert(execution_modes->Words());
    insert(debug->Words());
    insert(annotations->Words());
    insert(declarations->Words());
    insert(global_variables->Words());
    insert(code->Words());

    if (insert_index != words.size()) {
        throw std::logic_error{"Sirit SPIR-V assembly did not fill its output"};
    }

    return words;
}

void Module::PatchDeferredPhi(const std::function<Id(std::size_t index)>& func) {
    for (const u32 phi_index : deferred_phi_nodes) {
        const u32 first_word = code->Value(phi_index);
        [[maybe_unused]] const spv::Op op = static_cast<spv::Op>(first_word & 0xffff);
        assert(op == spv::Op::OpPhi);
        const u32 num_words = first_word >> 16;
        const u32 num_args = (num_words - 3) / 2;
        u32 cursor = phi_index + 3;
        for (u32 arg = 0; arg < num_args; ++arg, cursor += 2) {
            code->SetValue(cursor, func(arg).value);
        }
    }
}

void Module::AddExtension(std::string extension_name) {
    extensions.insert(std::move(extension_name));
}

void Module::AddCapability(spv::Capability capability) {
    capabilities.insert(capability);
}

void Module::SetMemoryModel(spv::AddressingModel addressing_model_,
                            spv::MemoryModel memory_model_) {
    addressing_model = addressing_model_;
    memory_model = memory_model_;
}

void Module::AddEntryPoint(spv::ExecutionModel execution_model, Id entry_point,
                           std::string_view name, std::span<const Id> interfaces) {
    entry_points->Reserve(4 + WordsInString(name) + interfaces.size());
    *entry_points << spv::Op::OpEntryPoint << execution_model << entry_point << name << interfaces
                  << EndOp{};
}

void Module::AddExecutionMode(Id entry_point, spv::ExecutionMode mode,
                              std::span<const Literal> literals) {
    execution_modes->Reserve(3 + literals.size());
    *execution_modes << spv::Op::OpExecutionMode << entry_point << mode << literals << EndOp{};
}

Id Module::AddLabel(Id label) {
    assert(label.value != 0);
    code->Reserve(2);
    *code << MakeWord0(spv::Op::OpLabel, 2) << label.value;
    return label;
}

Id Module::AddLocalVariable(Id result_type, spv::StorageClass storage_class,
                            std::optional<Id> initializer) {
    code->Reserve(5);
    return *code << OpId{spv::Op::OpVariable, result_type} << storage_class << initializer
                 << EndOp{};
}

Id Module::AddGlobalVariable(Id result_type, spv::StorageClass storage_class,
                             std::optional<Id> initializer) {
    global_variables->Reserve(5);
    return *global_variables << OpId{spv::Op::OpVariable, result_type} << storage_class
                             << initializer << EndOp{};
}

Id Module::GetGLSLstd450() {
    if (!glsl_std_450) {
        ext_inst_imports->Reserve(3 + 4);
        glsl_std_450 = *ext_inst_imports << OpId{spv::Op::OpExtInstImport} << "GLSL.std.450"
                                         << EndOp{};
    }
    return *glsl_std_450;
}

} // namespace Sirit
