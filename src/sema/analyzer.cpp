#include <sema/checker.hpp>
#include <sema/sema.hpp>


namespace Sema {

Analyzer::Analyzer(Types::TypeContext& types, ErrorReporting::ErrorReporter* reporter)
    : types_(types), reporter_(reporter) {}

SemaResult Analyzer::analyze(const std::shared_ptr<AST::ProgramRoot>& program,
                             const std::vector<FunctionInfo>& importedFunctions,
                             const std::vector<StructInfo>& importedStructs,
                             const std::vector<ClassInfo>& importedClasses,
                             const std::vector<EnumInfo>& importedEnums,
                             const std::vector<AST::ClassDeclaration*>& importedClassTemplates,
                             const std::vector<AST::FunctionDeclaration*>& importedFunctionTemplates,
                             const std::vector<SumTypeInfo>& importedSumTypes) {
    SemaResult result;

    const size_t errorsBefore =
        reporter_ ? reporter_->getDiagnostics().size() : 0;

    if (!program) {
        result.ok = (reporter_ == nullptr) ? true : !reporter_->hasError();
        return result;
    }

    result.moduleName = program->moduleName;

    Checker checker(types_, reporter_, result);
    checker.run(program, importedFunctions, importedStructs,
                importedClasses, importedEnums,
                importedClassTemplates, importedFunctionTemplates,
                importedSumTypes);

    bool newErrors = false;
    if (reporter_) {
        const auto& diags = reporter_->getDiagnostics();
        for (size_t i = errorsBefore; i < diags.size(); ++i) {
            if (diags[i].level == ErrorReporting::ErrorLevel::Error) {
                newErrors = true;
                break;
            }
        }
    }
    result.ok = !newErrors;
    return result;
}


Checker::Checker(Types::TypeContext& types, ErrorReporting::ErrorReporter* reporter,
                 SemaResult& result)
    : types_(types), reporter_(reporter), result_(result) {}

void Checker::run(const std::shared_ptr<AST::ProgramRoot>& program,
                  const std::vector<FunctionInfo>& importedFunctions,
                  const std::vector<StructInfo>& importedStructs,
                  const std::vector<ClassInfo>& importedClasses,
                  const std::vector<EnumInfo>& importedEnums,
                  const std::vector<AST::ClassDeclaration*>& importedClassTemplates,
                  const std::vector<AST::FunctionDeclaration*>& importedFunctionTemplates,
                  const std::vector<SumTypeInfo>& importedSumTypes) {
    importedStore_ = importedFunctions;

    pushScope();

    // Register everything imported BEFORE the local declaration pre-pass, so that
    // local function signatures / field / payload types can resolve imported
    // types (including instantiating imported generics like `Vector<i32>`) during
    // the pre-pass. (declarePrepass resolves declared signatures eagerly.)

    // Imported generic templates (cross-module generics): make them available to
    // this module's type resolution / instantiation. Local declarations win, so
    // only register names not already defined here.
    for (AST::ClassDeclaration* tmpl : importedClassTemplates) {
        if (tmpl && genericClassTemplates_.find(tmpl->name) == genericClassTemplates_.end()) {
            genericClassTemplates_[tmpl->name] = tmpl;
        }
    }
    for (AST::FunctionDeclaration* tmpl : importedFunctionTemplates) {
        if (tmpl && genericTemplates_.find(tmpl->name) == genericTemplates_.end()) {
            genericTemplates_[tmpl->name] = tmpl;
        }
    }

    for (const auto& s : importedStructs) {
        bool present = false;
        for (const auto& existing : result_.structs) {
            if (existing.name == s.name) { present = true; break; }
        }
        if (!present) {
            types_.registerNamed(s.name, Types::Kind::Struct);
            result_.structs.push_back(s);
        }
    }

    for (const auto& c : importedClasses) {
        bool present = false;
        for (const auto& existing : result_.classes) {
            if (existing.name == c.name) { present = true; break; }
        }
        if (!present) {
            types_.registerNamed(c.name, Types::Kind::Class);
            result_.classes.push_back(c);
            classFields_[c.name] = c.fields;
        }
    }

    for (const auto& e : importedEnums) {
        bool present = false;
        for (const auto& existing : result_.enums) {
            if (existing.name == e.name) { present = true; break; }
        }
        if (!present) {
            types_.registerNamed(e.name, Types::Kind::Enum);
            result_.enums.push_back(e);
        }
        // An imported enum is laid out by its declared underlying type just as a
        // local one is; the importing module has to learn that width too.
        if (e.underlying && !e.underlying->isError()) {
            types_.registerEnumUnderlying(e.name, e.underlying->bitWidth,
                                          e.underlying->isSigned);
        }
        Types::TypeRef enumType = types_.namedType(Types::Kind::Enum, e.name);
        for (const auto& variant : e.variants) {
            enumConstants_[variant.first] = enumType;
        }
    }

    // Imported tagged-union (sum-type) metadata: the empty StructInfo + name are
    // brought in via importedStructs above; here we bring the variant metadata so
    // construction (`E.Variant(...)`) and `switch` resolve, and the backend can lay
    // the aggregate out. TypeRefs are valid because the whole build shares one
    // TypeContext.
    for (const auto& st : importedSumTypes) {
        bool present = false;
        for (const auto& existing : result_.sumTypes) {
            if (existing.name == st.name) { present = true; break; }
        }
        if (present) continue;
        types_.registerNamed(st.name, Types::Kind::Struct);
        bool haveStruct = false;
        for (const auto& s : result_.structs) {
            if (s.name == st.name) { haveStruct = true; break; }
        }
        if (!haveStruct) {
            StructInfo shell;
            shell.name = st.name;
            shell.isExported = st.isExported;
            result_.structs.push_back(shell);
            classFields_[st.name] = {};
        }
        result_.sumTypes.push_back(st);
    }

    // Now the local declaration pre-pass; signatures can see imported types.
    declarePrepass(program);

    for (const auto& fn : result_.functions) {
        functionTable_.emplace(fn.name, fn);
    }
    for (const auto& fn : importedStore_) {
        functionTable_.emplace(fn.name, fn);
        auto pos = fn.name.rfind('_');
        if (pos != std::string::npos && pos + 1 < fn.name.size()) {
            functionTable_.emplace(fn.name.substr(pos + 1), fn);
        }
    }

    for (const auto& g : result_.globals) {
        scopes_.front().vars[g.name] = g.type ? g.type : types_.errorType();
    }

    const size_t initialFunctionCount = result_.functions.size();
    for (size_t i = 0; i < initialFunctionCount; ++i) {
        FunctionInfo fn = result_.functions[i];
        if (fn.decl && fn.decl->hasBody) {
            checkFunction(fn);
        }
    }

    for (auto& pm : pendingMethods_) {
        if (pm.method) {
            checkClassMethod(pm.className, pm.classPtr, *pm.method);
        }
    }

    size_t checkedInstantiations = 0;
    for (; checkedInstantiations < result_.genericInstantiations.size();
         ++checkedInstantiations) {
        GenericInstantiation inst = result_.genericInstantiations[checkedInstantiations];
        checkInstantiationBody(inst);
    }

    for (size_t i = 0; i < pendingGenericMethods_.size(); ++i) {
        PendingGenericMethod pm = pendingGenericMethods_[i];
        checkGenericClassMethod(pm);
    }
    for (; checkedInstantiations < result_.genericInstantiations.size();
         ++checkedInstantiations) {
        GenericInstantiation inst = result_.genericInstantiations[checkedInstantiations];
        checkInstantiationBody(inst);
    }

    popScope();
}

}

