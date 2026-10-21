param(
    [string]$BuildDirectory = 'build/codex-esp01s',
    [switch]$HmiConnectionTest,
    [switch]$HmiSerialBridge
)

if ($HmiConnectionTest -and $HmiSerialBridge) { throw '连接测试与串口转发不能同时启用。' }
if ($HmiSerialBridge -and -not $PSBoundParameters.ContainsKey('BuildDirectory')) {
    $BuildDirectory = 'build/codex-screen-serial-bridge'
}
if ($HmiConnectionTest -and -not $PSBoundParameters.ContainsKey('BuildDirectory')) {
    $BuildDirectory = 'build/codex-screen-connection-test'
}

# 使用电脑已有的 STM32Cube 工具，不安装或修改全局环境。
$buildRoot = Split-Path -Parent $PSScriptRoot
$buildBundles = "$env:LOCALAPPDATA/stm32cube/bundles"
$buildCmake = Get-ChildItem "$buildBundles/cmake" -Recurse -Filter cmake.exe | Select-Object -First 1 -ExpandProperty FullName
$buildNinja = Get-ChildItem "$buildBundles/ninja" -Recurse -Filter ninja.exe | Select-Object -First 1 -ExpandProperty FullName
$buildGcc = Get-ChildItem "$buildBundles/gnu-tools-for-stm32" -Recurse -Filter arm-none-eabi-gcc.exe | Select-Object -First 1 -ExpandProperty FullName
if (-not $buildCmake -or -not $buildNinja -or -not $buildGcc) { throw '找不到已有的 STM32Cube 编译工具。' }
if (-not $HmiConnectionTest -and -not $HmiSerialBridge -and (Test-Path "$buildRoot/config/network.local.json")) {
    & python "$PSScriptRoot/configure_network.py"
    if ($LASTEXITCODE -ne 0) { throw '网络配置生成失败。' }
}
$buildOldPath = $env:PATH
try {
    $env:PATH = (Split-Path -Parent $buildGcc) + ';' + $env:PATH
    $buildArguments = @('-S', $buildRoot, '-B', "$buildRoot/$BuildDirectory", '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Debug', "-DCMAKE_MAKE_PROGRAM=$buildNinja")
    $buildArguments += "-DHMI_CONNECTION_TEST=$(if ($HmiConnectionTest) { 'ON' } else { 'OFF' })"
    $buildArguments += "-DHMI_SERIAL_BRIDGE=$(if ($HmiSerialBridge) { 'ON' } else { 'OFF' })"
    if (-not (Test-Path "$buildRoot/$BuildDirectory/CMakeCache.txt")) {
        $buildArguments += "-DCMAKE_TOOLCHAIN_FILE=$buildRoot/cmake/gcc-arm-none-eabi.cmake"
    }
    & $buildCmake @buildArguments
    if ($LASTEXITCODE -ne 0) { throw 'CMake 配置失败。' }
    & $buildCmake --build "$buildRoot/$BuildDirectory"
    if ($LASTEXITCODE -ne 0) { throw '编译失败。' }
} finally { $env:PATH = $buildOldPath }
