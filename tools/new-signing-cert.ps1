$ErrorActionPreference = 'Stop'

$subject = 'CN=Nintendall, O=Nintendall'
$pfx     = 'C:\Users\Exult\Documents\emulators\Nintendall-V3.0\build\nintendall-signing.pfx'
$cer     = 'C:\Users\Exult\Documents\emulators\Nintendall-V3.0\build\nintendall-signing.cer'
$pass    = 'nintendall-dev'

$existing = Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert -ErrorAction SilentlyContinue |
            Where-Object { $_.Subject -eq $subject }

if ($existing) {
    Write-Host "Reusing existing signing certificate: $($existing[0].Thumbprint)"
    $cert = $existing[0]
} else {
    Write-Host 'Creating code-signing certificate...'
    $cert = New-SelfSignedCertificate `
        -Type CodeSigningCert `
        -Subject $subject `
        -CertStoreLocation Cert:\CurrentUser\My `
        -KeyAlgorithm RSA -KeyLength 3072 -HashAlgorithm SHA256 `
        -KeyExportPolicy Exportable `
        -KeyUsage DigitalSignature `
        -NotBefore (Get-Date).AddDays(-1) `
        -NotAfter  (Get-Date).AddYears(10)
    Write-Host "Created: $($cert.Thumbprint)"
}

Export-Certificate -Cert $cert -FilePath $cer -Force | Out-Null
Export-PfxCertificate -Cert $cert -FilePath $pfx -Password (ConvertTo-SecureString $pass -AsPlainText -Force) -Force | Out-Null

Write-Host ''
Write-Host "Thumbprint : $($cert.Thumbprint)"
Write-Host "Subject    : $($cert.Subject)"
Write-Host "Expires    : $($cert.NotAfter)"
Write-Host "Public cert: $cer"
Write-Host "PFX        : $pfx"
$cert.Thumbprint | Set-Content 'C:\Users\Exult\AppData\Local\Temp\opencode\cert-thumb.txt'