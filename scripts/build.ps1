param(
    [string]$QtRoot = $env:QTDIR,
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')][string]$Configuration = 'Release',
    [switch]$SkipTests,
    [switch]$Deploy
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
if (-not $QtRoot) {
    $qmakeCommand = Get-Command qmake.exe -ErrorAction SilentlyContinue
    if ($qmakeCommand) { $QtRoot = (& $qmakeCommand.Source -query QT_INSTALL_PREFIX).Trim() }
}
if (-not $QtRoot -or -not (Test-Path -LiteralPath (Join-Path $QtRoot 'lib\cmake\Qt6\Qt6Config.cmake'))) {
    throw '请通过 -QtRoot 指定 Qt 的 MSVC 64 位安装目录，例如 D:\Qt6\6.7.3\msvc2019_64。'
}
if ((& (Join-Path $QtRoot 'bin\qmake.exe') -query QMAKE_XSPEC) -notmatch 'msvc') {
    throw '请选择 MSVC 版本的 Qt；MinGW 库不能用于此构建。'
}
$cmakeCommand = Get-Command cmake.exe -ErrorAction SilentlyContinue
$cmakePath = if ($cmakeCommand) { $cmakeCommand.Source } else {
    $qtInstallationRoot = Split-Path -Parent (Split-Path -Parent $QtRoot)
    Join-Path $qtInstallationRoot 'Tools\CMake_64\bin\cmake.exe'
}
if (-not (Test-Path -LiteralPath $cmakePath)) { throw '找不到 CMake，请安装 CMake 或将其加入 PATH。' }
$buildDirectory = Join-Path $projectRoot 'build-msvc'
& $cmakePath -S $projectRoot -B $buildDirectory -G 'Visual Studio 17 2022' -A x64 "-DCMAKE_PREFIX_PATH=$QtRoot" '-DBUILD_TESTING=ON'
if ($LASTEXITCODE -ne 0) { throw 'CMake 配置失败。' }
& $cmakePath --build $buildDirectory --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) { throw 'MSVC 构建失败。' }
if (-not $SkipTests) {
    $previousPath = $env:PATH
    try {
        $env:PATH = (Join-Path $QtRoot 'bin') + ';' + $previousPath
        & (Join-Path (Split-Path -Parent $cmakePath) 'ctest.exe') --test-dir $buildDirectory -C $Configuration --output-on-failure
        if ($LASTEXITCODE -ne 0) { throw '集成测试失败。' }
    } finally { $env:PATH = $previousPath }
}
if ($Deploy) {
    $previousVcDirectory = $env:VCINSTALLDIR
    try {
        $vswherePath = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
        if (Test-Path -LiteralPath $vswherePath) {
            $visualStudioDirectory = & $vswherePath -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
            if ($visualStudioDirectory) { $env:VCINSTALLDIR = (Join-Path $visualStudioDirectory.Trim() 'VC') + '\' }
        }
        & $cmakePath --install $buildDirectory --config $Configuration --prefix (Join-Path $projectRoot 'dist')
        if ($LASTEXITCODE -ne 0) { throw '部署失败。' }
    } finally { $env:VCINSTALLDIR = $previousVcDirectory }
}
Write-Host "构建完成：$buildDirectory\$Configuration\ByteMe.exe"
if ($Deploy) { Write-Host "可运行目录：$projectRoot\dist\ByteMe.exe" }
