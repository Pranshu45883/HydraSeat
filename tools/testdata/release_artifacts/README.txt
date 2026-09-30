HydraSeat release artifact preflight fixture note

The release-artifact self-test creates all artifact bytes in a temporary directory.
It uses deterministic PE-shaped test bytes and an unsigned PowerShell fixture only to
exercise manifest/checksum/SBOM/provenance mechanics. These bytes are never production
artifacts, signing evidence, clean-machine evidence, or release qualification.

No file from C:\HydraSeat\references is copied, imported, or treated as a dependency.
The current reviewed release policy declares zero bundled third-party redistributables;
that must change only through an explicit reviewed release/dependency decision.
