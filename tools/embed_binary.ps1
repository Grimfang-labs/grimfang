param(
    [Parameter(Mandatory = $true)]
    [string]$InputFile,

    [Parameter(Mandatory = $true)]
    [string]$OutputFile
)

$inputPath = [System.IO.Path]::GetFullPath($InputFile)
$outputPath = [System.IO.Path]::GetFullPath($OutputFile)
$outputDir = [System.IO.Path]::GetDirectoryName($outputPath)

if (-not [System.IO.Directory]::Exists($outputDir)) {
    [System.IO.Directory]::CreateDirectory($outputDir) | Out-Null
}

$bytes = [System.IO.File]::ReadAllBytes($inputPath)
$writer = [System.IO.StreamWriter]::new($outputPath, $false, [System.Text.UTF8Encoding]::new($false))

try {
    $writer.WriteLine('#include <cstddef>')
    $writer.WriteLine()
    $writer.WriteLine('namespace embedded_net {')
    $writer.WriteLine()
    $writer.WriteLine('alignas(64) extern const unsigned char g_stockwolf_net_001[] = {')

    for ($i = 0; $i -lt $bytes.Length; $i++) {
        if (($i % 12) -eq 0) {
            $writer.Write('    ')
        }

        $writer.Write(('0x{0:X2}, ' -f $bytes[$i]))

        if (($i % 12) -eq 11) {
            $writer.WriteLine()
        }
    }

    if (($bytes.Length % 12) -ne 0) {
        $writer.WriteLine()
    }

    $writer.WriteLine('};')
    $writer.WriteLine()
    $writer.WriteLine('extern const std::size_t g_stockwolf_net_001_size = sizeof(g_stockwolf_net_001);')
    $writer.WriteLine()
    $writer.WriteLine('} // namespace embedded_net')
}
finally {
    $writer.Dispose()
}
