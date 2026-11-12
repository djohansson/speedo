# vcpkg's shared binary cache (see docs/notes/build.md, "vcpkg's shared binary cache"): the packages' zip archives in a
# Backblaze B2 bucket, read by everyone over https (the bucket is public: no credentials and no tools), written by a
# maintainer through the AWS CLI's S3 support (vcpkg's x-aws source), so that a new clone restores what was built
# elsewhere instead of building it (LLVM alone is most of an hour). Both name an archive <prefix><abi>.zip, the package's
# ABI hash, which vcpkg's http source reads as {sha}.zip.

$BinaryCacheBucket = 'danjo-artifacts'
$BinaryCacheRegion = 'us-west-001'
$BinaryCacheEndpoint = "s3.$BinaryCacheRegion.backblazeb2.com"
$BinaryCachePrefix = 'speedo/vcpkg/'

# where an archive is read from (anonymously)
function Get-BinaryCacheUrl
{
	param([Parameter(Mandatory = $True)] [string] $Abi)
	"https://$BinaryCacheEndpoint/$BinaryCacheBucket/$BinaryCachePrefix$Abi.zip"
}

# VCPKG_BINARY_SOURCES: the local default cache, read and written; the shared one read (unless SPEEDO_BINARY_CACHE is 0);
# and written where SPEEDO_BINARY_CACHE_WRITE is set (the AWS CLI's credentials its own: AWS_ACCESS_KEY_ID and
# AWS_SECRET_ACCESS_KEY, or AWS_PROFILE; a B2 application key scoped to the bucket. Never in this repository)
function Get-BinaryCacheSources
{
	$Sources = @('clear', 'default,readwrite')
	if ($env:SPEEDO_BINARY_CACHE -ne '0')
	{
		$Sources += "http,https://$BinaryCacheEndpoint/$BinaryCacheBucket/$BinaryCachePrefix{sha}.zip,read"
		if ($env:SPEEDO_BINARY_CACHE_WRITE)
		{
			$Sources += "x-aws,s3://$BinaryCacheBucket/$BinaryCachePrefix,write"
		}
	}
	$Sources -join ';'
}

# LLVM_PORT_VERSION: the llvm port's version, which the clang triplets track in their packages' ABI
# (VCPKG_ENV_PASSTHROUGH). The compiler is that port's, a host package of the same install, which vcpkg's compiler
# tracking can't see (it hashes the compiler before building anything: VCPKG_DISABLE_COMPILER_TRACKING), so without it the
# shared cache would hand out packages built by an older clang against an older libc++ after an LLVM update. The version
# vcpkg installs: an overlay's (<root>/ports/llvm), else vcpkg.json's override, else the registry's baseline's
# (vcpkg-configuration.json), read from the registry's versions/baseline.json at that commit, downloaded once a baseline
# into downloads (VCPKG_DOWNLOADS)
function Get-LlvmPortVersion
{
	param(
		[Parameter(Mandatory = $True)] [string] $Root,
		[Parameter(Mandatory = $True)] [string] $Downloads
	)
	$Overlay = Join-Path $Root 'ports/llvm/vcpkg.json'
	if (Test-Path $Overlay)
	{
		$Port = Get-Content -Raw -Path $Overlay | ConvertFrom-Json
		return "$($Port.version)#$($Port.'port-version' ?? 0)"
	}
	$Override = (Get-Content -Raw -Path (Join-Path $Root 'vcpkg.json') | ConvertFrom-Json).overrides | Where-Object name -eq 'llvm'
	if ($Override)
	{
		return "$($Override.version)#$($Override.'port-version' ?? 0)"
	}
	$Registry = (Get-Content -Raw -Path (Join-Path $Root 'vcpkg-configuration.json') | ConvertFrom-Json).'default-registry'
	if ($Registry.repository -notmatch '^https://github\.com/([^/]+/[^/]+?)(\.git)?/?$')
	{
		throw "the default registry ($($Registry.repository)) isn't on GitHub: llvm's version is read from its versions/baseline.json there"
	}
	$Baseline = Join-Path $Downloads "vcpkg-baseline-$($Registry.baseline).json"
	if (!(Test-Path $Baseline))
	{
		Invoke-WebRequest -Uri "https://raw.githubusercontent.com/$($Matches[1])/$($Registry.baseline)/versions/baseline.json" -OutFile "$Baseline.partial" -MaximumRetryCount 3 -RetryIntervalSec 5
		Move-Item -Force -Path "$Baseline.partial" -Destination $Baseline
	}
	$Llvm = (Get-Content -Raw -Path $Baseline | ConvertFrom-Json -AsHashtable).default['llvm']
	if (!$Llvm)
	{
		throw "the registry's baseline $($Registry.baseline) has no llvm port"
	}
	"$($Llvm.baseline)#$($Llvm.'port-version' ?? 0)"
}

# FASTBuild's cache in the same bucket, a prefix of its own (the objects FBuild's cache plugin stores, see
# ports/fastbuild-s3cache): read anonymously and written by the same machines, through S3's API at the same endpoint. Its
# objects expire by age: prune-binary-cache.ps1 deletes what is older than $FastbuildCacheExpiryDays, and the plugin, where
# it writes, dates anew an object it retrieves once it is older than half that, so that what builds keep using stays
$FastbuildCachePrefix = 'speedo/fastbuild/'
$FastbuildCacheExpiryDays = 30
function Get-FastbuildCacheUrl
{
	"https://$BinaryCacheEndpoint/$BinaryCacheBucket/$FastbuildCachePrefix"
}
function Get-FastbuildCacheRegion
{
	$BinaryCacheRegion
}
function Get-FastbuildCacheRefreshDays
{
	$FastbuildCacheExpiryDays / 2
}
# the AWS CLI's settings for the bucket (not secrets): B2's S3 endpoint and region, for writing
function Get-BinaryCacheAwsEnvironment
{
	[ordered] @{
		AWS_ENDPOINT_URL_S3 = "https://$BinaryCacheEndpoint"
		AWS_DEFAULT_REGION = $BinaryCacheRegion
	}
}

# vcpkg's local archives (its default files source): VCPKG_DEFAULT_BINARY_CACHE, else the platform's cache directory
function Get-LocalBinaryCachePath
{
	if ($env:VCPKG_DEFAULT_BINARY_CACHE) { return $env:VCPKG_DEFAULT_BINARY_CACHE }
	if (!(Test-Path Variable:\IsWindows) -or $IsWindows)
	{
		if ($env:LOCALAPPDATA) { return Join-Path $env:LOCALAPPDATA 'vcpkg/archives' }
		return Join-Path $env:APPDATA 'vcpkg/archives'
	}
	if ($env:XDG_CACHE_HOME) { return Join-Path $env:XDG_CACHE_HOME 'vcpkg/archives' }
	Join-Path $HOME '.cache/vcpkg/archives'
}
