#include <sema/checker.hpp>
#include <sema/sema.hpp>


namespace Sema {

Analyzer::Analyzer(Types::TypeContext& types, ErrorReporting::ErrorReporter* reporter)
    : types_(types), reporter_(reporter) {}

SemaResult Analyzer::analyze(const std::shared_ptr<AST::ProgramRoot>& program,
                             const std::vector<FunctionInfo>& importedFunctions,
                             const std::vector<StructInfo>& importedStructs) {
    SemaResult result;

    const size_t errorsBefore =
        reporter_ ? reporter_->getDiagnostics().size() : 0;

    if (!program) {
        result.ok = (reporter_ == nullptr) ? true : !reporter_->hasError();
        return result;
    }

    result.moduleName = program->moduleName;

    Checker checker(types_, reporter_, result);
    checker.run(program, importedFunctions, importedStructs);

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
                  const std::vector<StructInfo>& importedStructs) {
    importedStore_ = importedFunctions;

    pushScope();

    declarePrepass(program);

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

    for (const auto& fn : result_.functions) {
        functionTable_.emplace(fn.name, &fn);
    }
    for (const auto& fn : importedStore_) {
        functionTable_.emplace(fn.name, &fn);
        auto pos = fn.name.rfind('_');
        if (pos != std::string::npos && pos + 1 < fn.name.size()) {
            functionTable_.emplace(fn.name.substr(pos + 1), &fn);
        }
    }

    for (const auto& g : result_.globals) {
        scopes_.front().vars[g.name] = g.type ? g.type : types_.errorType();
    }

    for (const auto& fn : result_.functions) {
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
