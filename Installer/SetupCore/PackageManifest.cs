namespace GamEngine.Setup.Core;

public sealed class PackageManifest
{
    public string Version { get; set; } = "";
    public List<PackageFile> Files { get; set; } = [];
}

public sealed class PackageFile
{
    public string Path { get; set; } = "";
    public long Size { get; set; }
    public string Sha256 { get; set; } = "";
}

public sealed class CurrentVersion
{
    public string Version { get; set; } = "";
    public string? PreviousVersion { get; set; }
}

public sealed record InstallationProgress(int Percent, string Message);
