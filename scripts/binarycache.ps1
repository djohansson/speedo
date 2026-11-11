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

# FASTBuild's cache in the same bucket, a prefix of its own (the objects FBuild's cache plugin stores, see
# ports/fastbuild-s3cache): read anonymously and written by the same machines, through S3's API at the same endpoint; its
# entries expire by the bucket's lifecycle rule for the prefix, not by prune-binary-cache.ps1
$FastbuildCachePrefix = 'speedo/fastbuild/'
function Get-FastbuildCacheUrl
{
	"https://$BinaryCacheEndpoint/$BinaryCacheBucket/$FastbuildCachePrefix"
}
function Get-FastbuildCacheRegion
{
	$BinaryCacheRegion
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
