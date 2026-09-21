param(
    [Parameter(Mandatory=$true)][string]$BoostRoot,
    [Parameter(Mandatory=$true)][string]$OpenSSLRoot,
    [Parameter(Mandatory=$true)][string]$MySQLRoot,
    [Parameter(Mandatory=$true)][string]$MySQLExecutable,
    [string]$Source=(Join-Path $PSScriptRoot 'WER_Source'),
    [string]$Build=(Join-Path $PSScriptRoot 'build')
)
$ErrorActionPreference='Stop'
$env:Boost_ROOT=$BoostRoot
cmake -S $Source -B $Build -G 'Visual Studio 17 2022' -A x64 -DSCRIPTS=static -DMODULES=static "-DBOOST_LIBRARYDIR=$BoostRoot/lib64-msvc-14.3" "-DOPENSSL_ROOT_DIR=$OpenSSLRoot" "-DMYSQL_ROOT_DIR=$MySQLRoot" "-DMYSQL_EXECUTABLE=$MySQLExecutable" '-DDISABLED_AC_MODULES=mod-ollama-chat' '-DMODULE_MOD-OLLAMA-CHAT=disabled'
if($LASTEXITCODE -ne 0){throw 'CMake configuration failed'}
cmake --build $Build --config Release --target worldserver authserver --parallel 1 -- /p:CL_MPCount=1 /p:MultiProcMaxCount=1 /p:UseMultiToolTask=true /nologo
if($LASTEXITCODE -ne 0){throw 'Build failed'}
