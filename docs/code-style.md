# Code style

These rules come from MatrixGui (C++20), where they were settled by repeated
review. They apply to every project unless its own `CLAUDE.md` says otherwise.
Sections 1–4 hold in any language. Section 5 is specific to C++. Where the
existing code of a project disagrees with these rules, match the newest,
most-reviewed module rather than the oldest.

## 1. Comments: as few as possible, ideally none

Explanations belong in the module's README, not in the source.

Allowed in code:
- a short technical note that the code itself cannot tell you: a byte offset, a
  units convention, a platform quirk, where a fixture's numbers come from;
- a genuinely exceptional decision, where the obvious reading of the code is
  wrong and the reason is not written anywhere else. One or two lines.
- the closing `}  // namespace Name`.

Not allowed:
- doc comments on every function or method (`// Returns the gradient...`);
- restating what the next line does;
- trailing comments lined up after struct fields to explain each one;
- design rationale, "why not the other approach", history, or anything that is
  really a paragraph of documentation;
- commented-out code, `TODO` without an owner, and banner separators.

When you edit code and feel the need to explain it, update the README instead.
If a name needs a comment, rename it first.

## 2. Documentation lives next to the code, one README per module

Every folder that is a module of its own has a `README.md`. The top-level
`README.md` says what the project is, gives a table of modes or tools, a quick
start, and an index into `docs/`. `docs/` holds the narrative documentation:
pipelines, CLI reference, tuning. Module READMEs are the file-level reference.

A module README follows this shape:

1. `# \`path/to/module\` — what it is`, then one paragraph: what the folder
   does, what it does *not* do, and which other folder does that.
2. A file table:

   ```markdown
   | File | Contains |
   |---|---|
   | `error.h` | `Error` and its three children |
   | `model_loader.h/.cpp` | `LoadModel` — one directory into one model |
   ```

3. Layout and dependency direction when there are subfolders. Show which
   headers may include which, and state any module-wide invariant as something
   you can check with one command:

   ```bash
   grep -rln 'include <iostream>' core/words/    # must print nothing
   ```

4. One section per file or unit. Start with the public signatures in a code
   block, where short trailing comments are fine (`// nullopt when absent`).
   Then say what the unit guarantees, and end with a **Traps:** list of what
   will bite the next person.
5. Binary and file formats as a byte layout (`u32 magic`, `u64 count`,
   `f32[count]`), plus what `Load` refuses.
6. The exception tree of the module, and which failures are data, not
   exceptions.
7. A **Not here** section when readers are likely to look for something in the
   wrong place.

Writing rules for READMEs:
- State each fact in one place. Link to another README rather than repeat it.
- Be concrete. Give real numbers, names and limits, not "fast" or "large".
- When you quote a measurement, say how it was measured: build type, flags,
  hardware, tight loop or real workload. Never quote numbers from a Debug
  build as performance. Do not overclaim: if something serialises under
  contention, never write "never blocks".
- Update the README in the same change as the code it describes.

## 3. Structure

- **Folder = module = namespace = build target.** Each module has one
  namespace (`Words`, `Serving`, `Io`) and one library target. The target
  lists its dependencies explicitly, keeping public and private ones apart.
  The README states what the module deliberately does *not* link.
- **Core logic has no UI.** `core/` never writes to the console or opens a
  window. Anything that produces output takes a `std::ostream&` (or the
  language's equivalent sink). Front ends (`cli/`, `gui/`, HTTP) are thin: they
  parse input, call core, and render results. They add no logic of their own.
- **Pure functions apart from I/O.** For example, request handlers are
  `(snapshot, request) -> reply` with no sockets. The server is the one file
  that touches the network library. That makes every branch testable without
  a process or a port, and the expensive-to-compile dependency is paid for by
  one translation unit.
- **`main` only parses and dispatches.** Command bodies live in a library, so
  tests can link them without a second `main`. Each command has the signature
  `int Name(std::ostream& out, std::ostream& err, const NameOptions& options)`,
  where `NameOptions` is a plain struct of fields with defaults.
- **Dependencies point one way.** Document the order and never include
  upward. Forward-declare heavy types in headers. Put serialisation glue in a
  separate header (`config_json.h`) so a heavy third-party header is included
  only where you actually serialise.
- **Split by stage, not by kind.** Use `data/`, `train/`, `query/`, `report/`,
  not `utils/` or `helpers/`. Reporting and printing sit apart from
  computation: compute functions return structs, and `report/` renders them.
- Vendored third-party code goes under `contrib/`, untouched.

## 4. Behaviour

**Errors.**
- Anything that is nobody's fault at the keyboard throws: a broken file, an
  impossible configuration, a contract mismatch.
- Each module has its own hierarchy, rooted at `Module::Error`, with one child
  per way a caller would react differently.
- Expected misses from user input (an unknown word, a model that is not
  loaded) are **data**: a status enum or a struct with `ok` and `message`.
  They are not exceptions.
- Error messages carry the actual values and the path:
  `"manifest declares input.size 784, but the network takes 100"`,
  not `"size mismatch"`.
- At a module boundary, translate foreign exceptions into the module's own
  type, so the header's promise holds.
- Catch once at the top: one `Guarded` wrapper per CLI maps exceptions to exit
  codes. Exit codes are named constants (`kSuccess = 0`, `kFailure = 1` for a
  reported error, `kInternalError = 2` for a bug) and are documented.

**Validation.**
- Validate early, next to the setting that causes the problem, not deep inside
  the code that trips over it.
- Treat every size read from a file or a request as untrusted. Bound it, or
  cross-check it against the real file size, *before* you allocate.
- Refuse unknown keys in config and manifest formats. If a format needs an
  escape hatch, give it one explicit field for that.

**Determinism.**
- Output must not depend on hash-map iteration order or filesystem order.
  Sorts need a total order (break ties explicitly). Sort directory listings
  before acting on them. Prefer an ordered map wherever the order is visible
  in output.
- Seeds are fixed and written down wherever a run has to be reproducible.

**File formats.**
- Explicit little-endian, through shared read/write helpers.
- New formats start with a magic number and a version.
- The layout is documented byte by byte in the README.

**Names.** Use whole words. For example, `generatorLearningRate`, not
`generatorLr`; `vocabulary`, not `voc`. Established abbreviations of the
domain are fine (`id`, `rng`, `dim`, `sha256`). Name an ignored error
`ignored`, and one you inspect `failed`.

## 5. C++ specifics

**Language and build**
- Use C++20. The build must stay clean under `-Wall -Wextra`: fix warnings,
  do not silence them.
- Use `#pragma once`. Declarations go in `.h`, definitions in `.cpp`. Only
  trivial one-line accessors and templates are defined in headers.

**Naming**

| What | Style | Example |
|---|---|---|
| Free functions, static member functions | `PascalCase` | `LoadModel`, `Vocabulary::Build` |
| Methods | `camelCase` | `snapshot()`, `getWord(id)`, `requestStop()` |
| Types, enums, enum values | `PascalCase` | `enum class LookupStatus { Found, UnknownModel }` |
| Type aliases, concepts | `T` + `PascalCase` | `using TWordId = std::uint32_t;`, `concept TLayer` |
| Constants | `k` + `PascalCase` | `constexpr std::uintmax_t kMaxManifestBytes = 1u << 20;` |
| Namespaces | `PascalCase`, one level | `Serving`, and `ServingCli` for its front end |
| Members, locals, parameters | `camelCase`, no `m_` or trailing `_` | `publishMutex`, `stopRequested` |
| Files | `snake_case` | `model_loader.h`, `model_loader_test.cpp` |

If an accessor would clash with its field, the field gets a descriptive suffix
(`manifestData` behind `manifest()`, `directoryPath` behind `directory()`).
Pick either `getX()` or bare `x()` for accessors, and keep one style within a
module.

**Formatting**
- Indent 4 spaces. The opening brace goes on the same line. Always use braces,
  even around a one-line `if`.
- Keep lines at about 100 characters, with a hard limit near 110.
- When a signature does not fit on one line, put one parameter per line and
  the closing parenthesis on its own line:

  ```cpp
  LoadedModel(
      ModelManifest manifest,
      std::shared_ptr<const Neural::NeuralNetwork> network,
      std::filesystem::path directory,
      IntegrityCheck integrity
  );
  ```

- Constructor initializer lists put the comma first, and an empty body is
  written `{ }`:

  ```cpp
  ModelRegistry::ModelRegistry(RegistryConfig config)
      : slot(std::make_shared<const RegistrySnapshot>())
      , configuration(std::move(config))
  { }
  ```

- When a long string concatenation wraps, the `+` starts the continuation line.
- Use digit separators in large literals: `20'000'000`.
- Close a namespace with `}  // namespace Name` (two spaces before `//`). Put
  file-local helpers in an anonymous namespace at the top of the `.cpp`.

**Includes.** Arrange them in groups separated by blank lines:
1. the file's own header;
2. project headers, written as full paths from the repository root
   (`"core/serving/error.h"`);
3. third-party headers;
4. standard headers.

Sort each group alphabetically. Include what you use, and prefer `<iosfwd>`
and forward declarations in headers.

**Types and APIs**

- Use `static_cast<T>(value)` for explicit numeric, enum and compatible pointer conversions,
  including conversions previously written as `int(value)` or `Enum(value)`. Use
  `reinterpret_cast` for pointer reinterpretation and `const_cast` for cv-qualifier changes.
  Functional notation remains appropriate for object construction.
- Data is a plain `struct` with default member initializers. Configs, options,
  reports and results are all plain structs.
- A function returns its result by value. Out-parameters are only for an
  optional side channel (`Stats* stats = nullptr`).
- Use `std::optional` for "may be absent", `std::span` and `std::string_view`
  for views, and `enum class` rather than a `bool` or a string for a kind.
- Share immutable data as `std::shared_ptr<const T>`. Make a class immutable
  by giving it no mutators, not by giving it `const` members (those break
  move).
- Mark single-argument constructors `explicit`. Delete the copy operations of
  classes that own threads, mutexes or other resources.
- Write `const` on by-value parameters in the definition only, not in the
  declaration.
- Pin API contracts with `static_assert` where the compiler can check them
  (a layer satisfies its concept, an accessor returns by value).
- Every module's exceptions follow this pattern:

  ```cpp
  class Error : public std::runtime_error {
  public:
      using std::runtime_error::runtime_error;
  };

  class ManifestError : public Error {
  public:
      using Error::Error;
  };
  ```

## 6. Tests

- There are two layers:
  - unit tests, using doctest in C++;
  - golden snapshots of each CLI, pinning stdout, stderr and the exit code
    after normalisation (drop timings and other non-deterministic lines).
- Test-case names are sentences about behaviour, for example
  `TEST_CASE("Vocabulary::Build drops words below minCount")`.
- Test helpers go in an anonymous namespace. Shared fixtures go in
  `tests/support/`, in namespace `Tests`.
- CLI tests call the command functions directly, with `std::ostringstream` for
  `out` and `err`. They do not spawn a process.
- **Tests come before refactoring.** A refactor starts by pinning current
  behaviour with characterization tests or golden snapshots. That is a
  separate first step, not a check at the end.
- Never re-record an existing expectation to make a change pass. Adding new
  cases is fine. If an expectation really has to change, say so and explain
  why.
