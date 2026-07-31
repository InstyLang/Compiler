#pragma once

namespace Sema {
struct SemaResult;
}

namespace Types {
class TypeContext;
}

// Parses and semantically analyzes the internal core runtime module (the
// __ins_* helper functions: string formatters, allocator shims, etc.) against
// `types`, returning its SemaResult. The result is cached per TypeContext.
const Sema::SemaResult* getCoreRuntimeModule(Types::TypeContext& types);
