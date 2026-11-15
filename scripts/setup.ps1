$ErrorActionPreference = 'Stop'
Set-Location (Join-Path $PSScriptRoot '..')
New-Item -ItemType Directory '.deps' -Force | Out-Null

function Get-Sdk($Repository, $Tag, $Location, $Commit) {
    # 标签负责好找，提交号负责认人。上游挪了标签，也不能悄悄换一套 SDK。
    if (-not (Test-Path -LiteralPath $Location)) {
        git -c advice.detachedHead=false clone --depth 1 --branch $Tag --recurse-submodules --shallow-submodules $Repository $Location
        if ($LASTEXITCODE) { throw "Could not fetch $Repository" }
    }
    if ((git -C $Location rev-parse HEAD) -ne $Commit) { throw "Unexpected revision in $Location" }
    git -C $Location submodule update --init --recursive
    if ($LASTEXITCODE) { throw "Could not fetch submodules for $Location" }
}

Get-Sdk 'https://github.com/Celemony/ARA_SDK.git' 'releases/2.3.0' '.deps/ARA_SDK' '0005c532bb03eb210e7bb490329fc6d7ba1792c7'
Get-Sdk 'https://github.com/steinbergmedia/vst3sdk.git' 'v3.8.1_build_84' '.deps/vst3sdk' '3cdf9ca5d1f5b1b21e0a86832aa4abe55607bd96'
Get-Sdk 'https://github.com/TsudaKageyu/minhook.git' 'v1.3.4' '.deps/minhook' 'c3fcafdc10146beb5919319d0683e44e3c30d537'

$patchbay = 'BML-0.36.0-dev.14-windows'
if (-not (Test-Path ".deps/$patchbay/modkit/bml-modkit.jar")) {
    $zip = ".deps/$patchbay.zip"
    curl.exe -fLsS --retry 2 -o $zip "https://github.com/shaboogen-dev/Patchbay/releases/download/0.36.0-dev.14/$patchbay.zip"
    if ($LASTEXITCODE) { throw 'Could not fetch Patchbay.' }
    if ((Get-FileHash -LiteralPath $zip).Hash -ne '0b79642fb2b8c9d1bd22d18fd7041bcb0db7e75062769f8fd38f88fe9a4e5160') {
        throw 'Unexpected Patchbay archive SHA-256.'
    }
    tar -xf $zip -C .deps
    if ($LASTEXITCODE) { throw 'Could not extract Patchbay.' }
}
