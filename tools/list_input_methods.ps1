<#
.SYNOPSIS
  列出本机已安装的 TSF 输入法（TIP），输出可直接粘贴到 SimpleIME.toml 的 default_input_method 身份串。

.DESCRIPTION
  身份串格式（与 SimpleIME 插件 InputMethodIdentity 一致）：
      tip:{CLSID}/{GUID}/{langid}
  langid 为 4 位十六进制（可省略，省略则不校验语言）。

  数据来源：HKLM / HKCU 的 SOFTWARE\Microsoft\CTF\TIP 注册表，也就是插件的 EnumProfiles 看到的同一批 TSF 语言配置。
  显示名通过 SHLoadIndirectString 解析 MUI 资源，因此得到的是「微软拼音」这类本地化名称；
  同一个输入法可能同时登记在 HKLM 与 HKCU 下（其中一份只有 Enable 标记、没有名称），脚本会按身份串合并。

.EXAMPLE
  .\list_input_methods.ps1
  只列出来，自己复制身份串。

.EXAMPLE
  .\list_input_methods.ps1 -TomlPath "E:\Skyrim AE\mods\SimpleIME\interface\SimpleIME\SimpleIME.toml"
  列出，并显示该 TOML 当前的取值。

.EXAMPLE
  .\list_input_methods.ps1 -TomlPath "（同上路径）" -Select 2
  把列表里第 2 项直接写进 TOML，重启游戏生效。
#>
[CmdletBinding()]
param(
    [string] $TomlPath = '',
    [int]    $Select   = 0,
    [string] $Name     = '',
    [switch] $EnabledOnly,
    [switch] $IncludeNonIme
)

$ErrorActionPreference = 'Stop'

Add-Type -Namespace Win32 -Name Mui -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("shlwapi.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
public static extern int SHLoadIndirectString(string pszSource, System.Text.StringBuilder pszOutBuf, int cchOutBuf, System.IntPtr ppvReserved);
'@ -ErrorAction SilentlyContinue

function Resolve-MuiText([string] $text) {
    if ([string]::IsNullOrWhiteSpace($text)) { return '' }
    if (-not $text.StartsWith('@')) { return $text }
    $buffer = New-Object System.Text.StringBuilder 512
    $hr = [Win32.Mui]::SHLoadIndirectString($text, $buffer, 512, [System.IntPtr]::Zero)
    if ($hr -eq 0) { return $buffer.ToString() }
    return ''
}

function Get-RegValue($key, [string] $valueName) {
    if ($null -eq $key) { return $null }
    try { return $key.GetValue($valueName) } catch { return $null }
}

# 0x00000804 -> 0804；只保留低 16 位语言 id
function ConvertTo-LangId([string] $raw) {
    $hex = $raw -replace '^0x', ''
    if ($hex.Length -gt 4) { $hex = $hex.Substring($hex.Length - 4) }
    return [uint32]::Parse($hex, [System.Globalization.NumberStyles]::HexNumber)
}

$roots = @(('HKLM:' + 'SOFTWARE\Microsoft\CTF\TIP'), ('HKCU:' + 'SOFTWARE\Microsoft\CTF\TIP'))
$map   = [ordered] @{}

foreach ($root in $roots) {
    if (-not (Test-Path $root)) { continue }
    foreach ($tipKey in (Get-ChildItem $root -ErrorAction SilentlyContinue)) {
        $clsid = $tipKey.PSChildName.Trim('{}')
        if ($clsid -notmatch '^[0-9a-fA-F]{8}-') { continue }

        $langRoot = Join-Path $tipKey.PSPath 'LanguageProfile'
        if (-not (Test-Path $langRoot)) { continue }

        foreach ($langKey in (Get-ChildItem $langRoot -ErrorAction SilentlyContinue)) {
            if ($langKey.PSChildName -notmatch '^0x[0-9a-fA-F]+$') { continue }
            $langId  = ConvertTo-LangId $langKey.PSChildName
            $langHex = '{0:x4}' -f $langId

            foreach ($profileKey in (Get-ChildItem $langKey.PSPath -ErrorAction SilentlyContinue)) {
                $guid = $profileKey.PSChildName.Trim('{}')
                if ($guid -notmatch '^[0-9a-fA-F]{8}-') { continue }

                $descRaw   = [string](Get-RegValue $profileKey 'Description')
                $dispRaw   = [string](Get-RegValue $profileKey 'Display Description')
                $enableRaw = Get-RegValue $profileKey 'Enable'
                $enabled   = if ($null -eq $enableRaw) { $true } else { ([int]$enableRaw -ne 0) }

                $identity = 'tip:{{{0}}}/{{{1}}}/{2}' -f $clsid.ToUpperInvariant(), $guid.ToUpperInvariant(), $langHex
                $displayName = Resolve-MuiText $dispRaw
                if ([string]::IsNullOrWhiteSpace($displayName)) { $displayName = $descRaw }

                if (-not $map.Contains($identity)) {
                    $map[$identity] = [pscustomobject] @{
                        Name     = $displayName
                        English  = $descRaw
                        LangId   = $langHex
                        Enabled  = $enabled
                        Identity = $identity
                    }
                } else {
                    $row = $map[$identity]
                    if ([string]::IsNullOrWhiteSpace($row.Name) -and -not [string]::IsNullOrWhiteSpace($displayName)) { $row.Name = $displayName }
                    if ([string]::IsNullOrWhiteSpace($row.English) -and -not [string]::IsNullOrWhiteSpace($descRaw)) { $row.English = $descRaw }
                    $row.Enabled = $row.Enabled -or $enabled
                }
            }
        }
    }
}

if ($map.Count -eq 0) {
    Write-Host '没有枚举到任何 TSF 文本服务（CTF TIP 注册表为空）。' -ForegroundColor Yellow
    exit 1
}

$visible = @($map.Values |
    Where-Object { $IncludeNonIme -or ($_.English -notmatch '^(Tablet PC|Speech Recognition)') } |
    Where-Object { -not $EnabledOnly -or $_.Enabled } |
    Sort-Object @{ Expression = { if ($_.Enabled) { 0 } else { 1 } } }, @{ Expression = { if ([string]::IsNullOrWhiteSpace($_.Name)) { 1 } else { 0 } } }, Name, Identity)

Write-Host ''
Write-Host '本机已安装的输入法（TSF 语言配置）：' -ForegroundColor Cyan
$index = 0
foreach ($row in $visible) {
    $index++
    $label = if ([string]::IsNullOrWhiteSpace($row.Name)) { '(无显示名)' } else { $row.Name }
    $mark  = if ($row.Enabled) { '  ' } else { ' !' }
    Write-Host ('{0,2}.{1} {2}' -f $index, $mark, $label) -ForegroundColor White
    Write-Host ('      {0}' -f $row.Identity) -ForegroundColor DarkGray
    if ($row.English -and $row.English -ne $row.Name) {
        Write-Host ('      [{0}]' -f $row.English) -ForegroundColor DarkGray
    }
}
Write-Host ''
$sample = @($visible | Where-Object { $_.Enabled -and -not [string]::IsNullOrWhiteSpace($_.Name) }) | Select-Object -First 1
if ($null -eq $sample) { $sample = $visible[0] }
Write-Host '下一步：把想用的那一行身份串复制到 SimpleIME.toml 的 [core] 段，或直接让脚本写进去：' -ForegroundColor Cyan
Write-Host ('  .\list_input_methods.ps1 -TomlPath "<SimpleIME.toml>" -Name "{0}"' -f $(if ($sample.Name) { $sample.Name } else { '微信' })) -ForegroundColor DarkGray
Write-Host ''

if ($TomlPath -and (Test-Path $TomlPath)) {
    $bytes  = [System.IO.File]::ReadAllBytes($TomlPath)
    $hasBom = ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF)
    $utf8   = New-Object System.Text.UTF8Encoding($hasBom)
    $text   = [System.IO.File]::ReadAllText($TomlPath, [System.Text.Encoding]::UTF8)

    $pattern = '(?m)^[ \t]*default_input_method[ \t]*=[ \t]*"(.*)"'
    $current = [regex]::Match($text, $pattern)
    if ($current.Success) {
        Write-Host ('当前 default_input_method = "{0}"' -f $current.Groups[1].Value) -ForegroundColor DarkGray
    } else {
        Write-Host '当前 TOML 里还没有 default_input_method 这一行。' -ForegroundColor DarkGray
    }

    if (-not [string]::IsNullOrWhiteSpace($Name)) {
        $needle  = $Name.ToLowerInvariant()
        $named   = @($visible | Where-Object { $_.Name -and $_.Name.ToLowerInvariant().Contains($needle) })
        if ($named.Count -eq 0) {
            Write-Host ('-Name "{0}" 没有匹配到任何输入法。' -f $Name) -ForegroundColor Red
            exit 1
        }
        if ($named.Count -gt 1) {
            Write-Host ('-Name "{0}" 匹配到 {1} 个输入法，请改用 -Select 编号：' -f $Name, $named.Count) -ForegroundColor Yellow
            foreach ($n in $named) { Write-Host ('  {0}  {1}' -f $n.Name, $n.Identity) -ForegroundColor DarkGray }
            exit 1
        }
        $Select = [array]::IndexOf($visible, $named[0]) + 1
        Write-Host ('-Name "{0}" -> {1}' -f $Name, $named[0].Name) -ForegroundColor Gray
    }

    if ($Select -ge 1) {
        if ($Select -gt $visible.Count) {
            Write-Host ('-Select {0} 超出范围（共 {1} 项）。' -f $Select, $visible.Count) -ForegroundColor Red
            exit 1
        }
        $newLine = 'default_input_method = "' + $visible[$Select - 1].Identity + '"'
        if ($current.Success) {
            $text = [regex]::Replace($text, $pattern, $newLine, 1)
        } else {
            $coreHeader    = '[' + 'core' + ']'
            $headerPattern = '(?m)^' + [regex]::Escape($coreHeader) + '[ \t]*$'
            $text = [regex]::Replace($text, $headerPattern, ($coreHeader + "`r`n" + $newLine), 1)
        }
        [System.IO.File]::WriteAllText($TomlPath, $text, $utf8)
        Write-Host ('已写入 {0}' -f $TomlPath) -ForegroundColor Green
        Write-Host ('  {0}' -f $newLine) -ForegroundColor Green
        Write-Host '重启游戏后生效。' -ForegroundColor Green
    }
}
