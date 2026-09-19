# Security Policy

## Supported versions

GamEngine is under active development. Security fixes are generally applied to the latest version of the `main` branch.

Older commits, experimental branches, local forks, and archived builds are not guaranteed to receive security updates.

| Version | Supported |
| --- | --- |
| Latest `main` | Yes |
| Older commits | No guarantee |
| Experimental branches | No guarantee |

## Reporting a vulnerability

Please do not open a public GitHub issue for a vulnerability that could enable exploitation, data exposure, arbitrary code execution, privilege escalation, unsafe file handling, or other security-sensitive behavior.

Instead, report the issue privately using GitHub Security Advisories if they are enabled for the repository.

When reporting a vulnerability, include as much of the following information as possible:

- a clear description of the issue;
- the affected commit SHA or version;
- the affected subsystem;
- steps required to reproduce the issue;
- proof-of-concept input, asset, scene, shader, or file where appropriate;
- expected behavior;
- actual behavior;
- operating system;
- compiler and build configuration;
- GPU and driver version when relevant;
- logs, validation messages, crash dumps, or stack traces;
- the estimated impact;
- any known workaround.

Please avoid including secrets, personal data, access tokens, credentials, or unrelated private information in the report.

## Scope

Security-relevant reports may include, but are not limited to:

- arbitrary code execution;
- memory corruption;
- buffer overflows;
- out-of-bounds reads or writes;
- use-after-free bugs;
- unsafe deserialization;
- malicious scene or asset files causing unsafe behavior;
- directory traversal;
- unsafe file extraction;
- command injection;
- shader or asset compilation paths that permit unintended command execution;
- unsafe plugin or script loading;
- DLL search-order or module-loading vulnerabilities;
- privilege escalation;
- sensitive information disclosure;
- unsafe temporary-file handling;
- denial-of-service issues caused by untrusted input;
- GPU resource exhaustion caused by malformed assets;
- integer overflows that can lead to memory safety problems;
- incorrect validation of external file offsets, counts, indices, or sizes.

## Out of scope

The following are normally not treated as security vulnerabilities unless they can be shown to create a concrete security impact:

- ordinary rendering artifacts;
- incorrect lighting or shadow output;
- FPS regressions;
- high GPU or CPU usage caused by expected workloads;
- Vulkan validation warnings without an exploitable consequence;
- crashes caused only by intentionally invalid internal developer code;
- unsupported drivers or unsupported hardware;
- bugs requiring modification of the engine source and recompilation by the attacker;
- local debug builds with intentionally enabled diagnostics;
- missing hardening features without a demonstrated exploit;
- general feature requests.

## Untrusted assets and files

GamEngine should treat externally supplied files as untrusted input.

Code that parses or imports files should validate:

- file size;
- offsets;
- counts;
- indices;
- array bounds;
- string lengths;
- dimensions;
- mip counts;
- vertex and index counts;
- buffer sizes;
- allocation sizes;
- arithmetic overflow;
- relative and absolute paths;
- referenced external files;
- compression metadata;
- image metadata;
- scene hierarchy references.

Importers and serializers must not assume that data produced by third-party tools is valid.

Malformed input should fail safely with a useful error instead of causing memory corruption, uncontrolled allocation, or undefined behavior.

## Path handling

Code working with project files, assets, caches, generated files, or imported resources should avoid unsafe path construction.

When handling paths derived from external data:

- normalize paths before validation;
- reject directory traversal such as `../` where it is not explicitly allowed;
- avoid writing outside the intended project or cache directory;
- do not trust archive entry names;
- do not overwrite arbitrary user files;
- avoid following unexpected symbolic links when writing extracted data;
- validate destination paths before file creation.

## Native modules and scripts

GamEngine may load native modules or project scripts.

Native modules should be treated as trusted code.

Do not load arbitrary DLLs or native modules from untrusted locations.

When resolving native modules:

- prefer explicit absolute paths;
- avoid relying on the process working directory;
- avoid unsafe DLL search-order behavior;
- validate that the requested module belongs to the active project or trusted engine installation;
- unload modules only when no engine code can still call into them.

A malicious native module has the same privileges as the GamEngine process and is therefore outside any asset-level sandbox.

## Shader compilation

Shader source should be treated as source code, not as ordinary passive asset data.

Shader compilation code must not construct shell commands by concatenating untrusted input.

Prefer direct process invocation with separated arguments.

When compiling shaders:

- validate source and output paths;
- keep generated output inside known build or cache directories;
- avoid executing arbitrary command-line fragments from project assets;
- do not trust filenames as command-line options;
- quote or separate process arguments correctly;
- avoid inheriting unsafe environment-dependent executable lookup when possible.

## External tools

GamEngine may invoke external development tools such as CMake, Slang, compilers, or build tools.

Paths and arguments passed to external processes must not permit command injection.

Do not use an untrusted project string directly inside a shell command.

Prefer APIs that execute a process directly rather than passing a composed string to `cmd.exe`, PowerShell, or another shell.

## Vulkan and GPU robustness

GPU validation errors are normally correctness bugs, but some resource-lifetime and bounds errors can become security-relevant when processing untrusted input.

Rendering code should:

- validate buffer and image sizes;
- avoid out-of-bounds shader accesses;
- bound indirect draw and dispatch counts;
- validate externally derived mesh and material indices;
- prevent integer overflow in GPU buffer offset calculations;
- validate descriptor array indices;
- use bounds checks for dynamically indexed resources where necessary;
- avoid trusting asset-provided counts when allocating GPU resources.

GPU-generated counts should be clamped to the capacity of their destination buffers.

Indirect command buffers must never be allowed to reference memory beyond their allocated range.

## Resource exhaustion

Untrusted files should not be able to request effectively unlimited CPU, RAM, VRAM, descriptor, or GPU-work allocations.

Where practical, enforce reasonable limits for:

- texture dimensions;
- texture array layers;
- mip levels;
- mesh vertex counts;
- mesh index counts;
- material counts;
- entity counts;
- animation tracks;
- particle counts;
- shadow allocations;
- render-target dimensions;
- imported file sizes;
- decompressed data size.

Large valid projects may need high limits, but those limits should still be explicit and checked.

## Serialization

Scene and project deserialization must validate data before using it.

Do not assume:

- IDs are unique;
- referenced entities exist;
- parent relationships are valid;
- indices are in range;
- enum values are valid;
- component payloads contain expected values;
- serialized counts match the actual file contents.

Malformed relationships should be rejected or repaired safely without dereferencing invalid objects.

## Logging

Logs should help diagnose security issues without leaking sensitive data.

Avoid logging:

- access tokens;
- passwords;
- private keys;
- authentication cookies;
- secrets stored in environment variables;
- full contents of confidential project files.

File paths and local system information may be useful for debugging, but avoid collecting or transmitting them unnecessarily.

## Secrets

Do not commit secrets to the repository.

Examples include:

- API keys;
- passwords;
- private certificates;
- signing keys;
- access tokens;
- cloud credentials.

Use environment variables, local configuration, or an appropriate secret-management mechanism instead.

If a secret is accidentally committed, removing it from the latest commit is not sufficient. Revoke or rotate the credential immediately.

## Dependencies

Third-party dependencies should be kept reasonably current, especially when they process external input.

Security-sensitive dependency updates should be reviewed carefully for compatibility and behavior changes.

Avoid adding a dependency when the same result can be achieved safely with existing project facilities at a reasonable maintenance cost.

## Build artifacts

Do not treat build artifacts received from unknown sources as trusted.

Prefer building GamEngine from source or obtaining binaries from a trusted release source.

Debug and development packages may contain:

- symbols;
- source paths;
- diagnostic information;
- additional tooling;
- validation layers.

They should not automatically be treated as hardened production distributions.

## Security fixes

A security fix should ideally include:

- the minimal code change necessary to correct the issue;
- regression coverage when practical;
- validation of related code paths;
- no unrelated refactoring;
- documentation when the bug resulted from a non-obvious security invariant.

Security fixes should not intentionally expose exploit details before users have had a reasonable opportunity to update.

## Coordinated disclosure

If a vulnerability affects released or widely distributed builds, please allow time for a fix to be prepared before publishing detailed exploitation instructions.

Once a fix is available, the project may publish:

- affected versions or commits;
- severity;
- impact;
- mitigation;
- fixed version or commit;
- credit to the reporter, if desired.

## Security development checklist

For changes that process external or project-controlled data, consider the following before submitting:

- [ ] All externally derived counts are bounds checked.
- [ ] Integer calculations cannot overflow into smaller allocations.
- [ ] Buffer offsets and sizes are validated.
- [ ] Array and descriptor indices are validated.
- [ ] File paths cannot escape their intended root unexpectedly.
- [ ] External process arguments cannot inject shell commands.
- [ ] Malformed files fail safely.
- [ ] Excessive allocations are bounded.
- [ ] GPU indirect counts cannot exceed buffer capacity.
- [ ] Resource lifetimes remain valid for all in-flight frames.
- [ ] No credentials or secrets are logged or committed.
- [ ] Security-sensitive behavior has regression coverage where practical.

## Contact

For security-sensitive reports, use the repository's private GitHub security reporting mechanism when available.

For ordinary bugs that do not have security impact, use the public issue tracker.
