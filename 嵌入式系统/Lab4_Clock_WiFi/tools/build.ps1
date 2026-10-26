param([string]$CompilerRoot = '')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
if (-not $CompilerRoot) {
    $courseRoot = Split-Path (Split-Path $projectRoot -Parent) -Parent
    $CompilerRoot = Join-Path $courseRoot 'ccs\tools\compiler\ti-cgt-arm_20.2.7.LTS'
}
$compiler = Join-Path $CompilerRoot 'bin\armcl.exe'
if (-not (Test-Path -LiteralPath $compiler)) { throw 'TI ARM compiler not found; supply -CompilerRoot.' }
$buildRoot = Join-Path $projectRoot 'BuildCheck'
New-Item -ItemType Directory -Path $buildRoot -Force | Out-Null
$compilerFlags = @('-mv7M4','--code_state=16','--float_support=FPv4SPD16','-me','--abi=eabi','--gcc','--define=PART_TM4C123GH6PM','--define=TARGET_IS_BLIZZARD_RB1','--diag_warning=225','--diag_wrap=off','--display_error_number','-g',"--include_path=$projectRoot","--include_path=$CompilerRoot\include","--obj_directory=$buildRoot")
foreach ($sourceName in @('main.c','wifi_clock.c','uart_log.c','tm4c123gh6pm_startup_ccs.c')) {
    & $compiler @compilerFlags --compile_only (Join-Path $projectRoot $sourceName)
    if ($LASTEXITCODE -ne 0) { throw "Compilation failed: $sourceName" }
}
$linkFlags = @('-mv7M4','--code_state=16','--float_support=FPv4SPD16','--abi=eabi','-z','--rom_model','--heap_size=0','--stack_size=1024','--warn_sections','--diag_wrap=off','--display_error_number',"--output_file=$buildRoot\Lab4_Clock_WiFi.out","--map_file=$buildRoot\Lab4_Clock_WiFi.map","--xml_link_info=$buildRoot\Lab4_Clock_WiFi_linkInfo.xml","--search_path=$CompilerRoot\lib",(Join-Path $buildRoot 'main.obj'),(Join-Path $buildRoot 'wifi_clock.obj'),(Join-Path $buildRoot 'uart_log.obj'),(Join-Path $buildRoot 'tm4c123gh6pm_startup_ccs.obj'),(Join-Path $projectRoot 'tm4c123gh6pm.cmd'),(Join-Path $projectRoot 'driverlib\ccs\Debug\driverlib.lib'),'--library=rtsv7M4_T_le_v4SPD16_eabi.lib')
& $compiler @linkFlags
if ($LASTEXITCODE -ne 0) { throw 'Link failed.' }
Write-Output "Build passed: $buildRoot\Lab4_Clock_WiFi.out"
