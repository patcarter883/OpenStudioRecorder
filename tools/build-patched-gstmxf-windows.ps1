#Requires -Version 5.1
<#
.SYNOPSIS
    Builds a patched gstmxf.dll (MXF timecode seeding) for the Windows capture machine.

.DESCRIPTION
    Rebuilds ONLY the mxf plugin of gst-plugins-bad from the matching upstream source
    tarball, with patches/0001-mxfmux-seed-timecode-from-videotimecodemeta.patch applied.
    The resulting gstmxf.dll is dropped in -PluginOutDir and loaded via GST_PLUGIN_PATH,
    so the official GStreamer installation is left untouched.

    Prerequisites (all on the machine that runs this):
      * Official GStreamer installer, installed with the development headers:
            gstreamer-1.0-msvc-x86_64-<ver>.exe /TYPE=devel /SILENT /NORESTART
        (this sets GSTREAMER_1_0_ROOT_MSVC_X86_64 and provides headers + .lib files)
      * Visual Studio 2022 (C++ workload)
      * Python 3 + meson + ninja  ( pip install meson ninja )
      * Git for Windows (provides `git apply`) — and Windows 10+ for bundled tar.exe

.PARAMETER GStreamerVersion
    Version to build against. MUST match the installed runtime:
        gst-launch-1.0 --version
    (the plugin must be built from the same version's source).

.EXAMPLE
    # from an "x64 Native Tools Command Prompt for VS 2022"
    .\build-patched-gstmxf-windows.ps1 -GStreamerVersion 1.28.6

.NOTES
    Not a substitute for shipping a patched runtime (use Cerbero for that, with the same
    patch carried in recipes/gst-plugins-bad-1.0/patches/). This is the fast local route.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$GStreamerVersion,
    [string]$PluginOutDir = (Join-Path $PSScriptRoot '..\build\patched-gst-plugins'),
    [string]$WorkDir      = (Join-Path $env:TEMP 'gst-mxf-patch')
)

$ErrorActionPreference = 'Stop'

# --- 1. sanity: is the official runtime + dev headers present? -----------------
$gstRoot = $env:GSTREAMER_1_0_ROOT_MSVC_X86_64
if (-not $gstRoot) {
    throw "GSTREAMER_1_0_ROOT_MSVC_X86_64 is not set. Install the official GStreamer installer with /TYPE=devel first."
}
if (-not (Test-Path (Join-Path $gstRoot 'include\gstreamer-1.0\gst\gst.h'))) {
    throw "No GStreamer headers found under $gstRoot - reinstall with /TYPE=devel (Runtime and development headers)."
}
foreach ($tool in 'meson', 'ninja', 'git') {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "$tool not found on PATH. (pip install meson ninja; Git for Windows for 'git apply'.)"
    }
}
Write-Host "GStreamer dev root : $gstRoot"
Write-Host "Building version   : $GStreamerVersion"

# --- 2. fetch + unpack the matching source tarball -----------------------------
New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null
$tarball = "gst-plugins-bad-$GStreamerVersion.tar.xz"
$srcDir  = Join-Path $WorkDir "gst-plugins-bad-$GStreamerVersion"
if (-not (Test-Path $srcDir)) {
    $dl = Join-Path $WorkDir $tarball
    if (-not (Test-Path $dl)) {
        $url = "https://gstreamer.freedesktop.org/src/gst-plugins-bad/$tarball"
        Write-Host "Downloading $url"
        Invoke-WebRequest -Uri $url -OutFile $dl
    }
    tar -xf $dl -C $WorkDir
}

# git apply works on a plain directory, but this makes the behaviour identical to a
# developer machine and keeps CI honest.
if (-not (Test-Path (Join-Path $srcDir '.git'))) { git init -q $srcDir | Out-Null }

# --- 3. apply our patch (fail fast if the base version is wrong) ---------------
$patchFile = Resolve-Path (Join-Path $PSScriptRoot '..\patches\0001-mxfmux-seed-timecode-from-videotimecodemeta.patch')
Push-Location $srcDir
try {
    git apply -p1 --check $patchFile
    if ($LASTEXITCODE -ne 0) { throw "Patch does not apply cleanly to $GStreamerVersion - check the base version." }
    git apply -p1 $patchFile
    Write-Host "Patch applied to $srcDir"
} finally {
    Pop-Location
}

# --- 4. configure + build just the mxf plugin ----------------------------------
# --vsenv makes meson activate the MSVC environment itself if we are not already
# running inside a Visual Studio developer prompt.
$buildDir = Join-Path $srcDir 'build'
meson setup $buildDir $srcDir --vsenv "--prefix=$WorkDir\prefix" -Dauto_features=disabled -Dmxf=enabled
if ($LASTEXITCODE -ne 0) { throw "meson setup failed (exit $LASTEXITCODE) - see the meson output above" }
ninja -C $buildDir
if ($LASTEXITCODE -ne 0) { throw "ninja failed (exit $LASTEXITCODE) - see the ninja output above" }

# --- 5. collect the plugin DLL --------------------------------------------------
New-Item -ItemType Directory -Force -Path $PluginOutDir | Out-Null
$dll = Get-ChildItem -Path $buildDir -Recurse -Filter 'gstmxf.dll' | Select-Object -First 1
if (-not $dll) { throw "gstmxf.dll not found under $buildDir - did the build fail?" }
Copy-Item $dll.FullName $PluginOutDir -Force

# --- 6. self-check: GStreamer must resolve mxfmux to the plugin we just built --
# Without this the artifact could silently be useless (e.g. if the plugin were refused
# for being built for an older core than the installed runtime).
$env:GST_PLUGIN_PATH = $PluginOutDir
$inspect = & (Join-Path $gstRoot 'bin\gst-inspect-1.0.exe') mxfmux 2>&1
$filenameLine = ($inspect | Select-String -Pattern '^\s*Filename\s+(.+)$' | Select-Object -First 1)
if (-not $filenameLine) { throw "gst-inspect-1.0 could not resolve mxfmux at all after the build." }
$resolved = $filenameLine.Matches[0].Groups[1].Value.Trim()
if ($resolved -notlike "$PluginOutDir*") {
    throw "GStreamer resolved mxfmux to '$resolved' instead of the built plugin under '$PluginOutDir'."
}
Write-Host "Self-check OK: mxfmux resolves to $resolved"

# --- 7. manifest: make every artifact self-describing --------------------------
$manifest = [ordered]@{
    gstreamer_source_version = $GStreamerVersion
    plugin                   = 'gstmxf.dll'
    built_utc                = (Get-Date).ToUniversalTime().ToString('o')
    patch_file               = (Split-Path $patchFile -Leaf)
    patch_sha256             = (Get-FileHash $patchFile -Algorithm SHA256).Hash
    plugin_sha256            = (Get-FileHash (Join-Path $PluginOutDir 'gstmxf.dll') -Algorithm SHA256).Hash
    gstreamer_root           = $gstRoot
}
$manifest | ConvertTo-Json | Set-Content -Path (Join-Path $PluginOutDir 'manifest.json') -Encoding utf8
Write-Host "Manifest: $PluginOutDir\manifest.json"

Write-Host ""
Write-Host "Patched plugin  : $PluginOutDir\gstmxf.dll"
Write-Host "Load it without touching the official install:"
Write-Host "    `$env:GST_PLUGIN_PATH = '$PluginOutDir'"
Write-Host ""
Write-Host "Verify which plugin is loaded (Filename must be the one above):"
Write-Host "    gst-inspect-1.0 mxfmux"
Write-Host ""
Write-Host "End-to-end check (timecode must NOT be 00:00:00:00):"
Write-Host "    gst-launch-1.0 -e videotestsrc num-buffers=50 ! video/x-raw,format=YV12,width=1920,height=1080,framerate=25/1 ! timecodestamper source=rtc ! videoconvert ! avenc_dnxhd profile=2 threads=1 ! mxfmux ! filesink location=test.mxf"
Write-Host "    ffmpeg -i test.mxf     # use ffmpeg, NOT ffprobe - ffprobe cannot show MXF timecode"
