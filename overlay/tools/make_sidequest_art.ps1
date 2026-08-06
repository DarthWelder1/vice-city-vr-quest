param(
	[Parameter(Mandatory = $true)]
	[string]$ListingSource,

	[Parameter(Mandatory = $true)]
	[string]$BannerSource,

	[Parameter(Mandatory = $true)]
	[string]$OutputDirectory
)

Add-Type -AssemblyName System.Drawing

function New-Canvas([int]$Width, [int]$Height) {
	$bitmap = [System.Drawing.Bitmap]::new(
		$Width,
		$Height,
		[System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
	$bitmap.SetResolution(96.0, 96.0)
	return $bitmap
}

function New-Graphics([System.Drawing.Bitmap]$Bitmap) {
	$graphics = [System.Drawing.Graphics]::FromImage($Bitmap)
	$graphics.SmoothingMode =
		[System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
	$graphics.InterpolationMode =
		[System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
	$graphics.PixelOffsetMode =
		[System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
	$graphics.CompositingQuality =
		[System.Drawing.Drawing2D.CompositingQuality]::HighQuality
	$graphics.TextRenderingHint =
		[System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit
	return $graphics
}

function Draw-CoverCrop(
	[System.Drawing.Graphics]$Graphics,
	[System.Drawing.Image]$Source,
	[int]$Width,
	[int]$Height,
	[double]$VerticalFocus = 0.5
) {
	$destinationAspect = $Width / [double]$Height
	$sourceAspect = $Source.Width / [double]$Source.Height

	if ($sourceAspect -gt $destinationAspect) {
		$cropHeight = $Source.Height
		$cropWidth = [int][Math]::Round($cropHeight * $destinationAspect)
		$cropX = [int][Math]::Round(($Source.Width - $cropWidth) / 2.0)
		$cropY = 0
	} else {
		$cropWidth = $Source.Width
		$cropHeight = [int][Math]::Round($cropWidth / $destinationAspect)
		$cropX = 0
		$availableY = $Source.Height - $cropHeight
		$cropY = [int][Math]::Round($availableY * $VerticalFocus)
	}

	$destination = [System.Drawing.Rectangle]::new(0, 0, $Width, $Height)
	$sourceRect = [System.Drawing.Rectangle]::new(
		$cropX, $cropY, $cropWidth, $cropHeight)
	$Graphics.DrawImage(
		$Source,
		$destination,
		$sourceRect,
		[System.Drawing.GraphicsUnit]::Pixel)
}

function Add-TopShade(
	[System.Drawing.Graphics]$Graphics,
	[int]$Width,
	[int]$Height,
	[int]$ShadeHeight,
	[int]$PeakAlpha
) {
	$rectangle = [System.Drawing.Rectangle]::new(0, 0, $Width, $ShadeHeight)
	$top = [System.Drawing.Color]::FromArgb($PeakAlpha, 3, 2, 12)
	$bottom = [System.Drawing.Color]::FromArgb(0, 3, 2, 12)
	$brush = [System.Drawing.Drawing2D.LinearGradientBrush]::new(
		$rectangle,
		$top,
		$bottom,
		[System.Drawing.Drawing2D.LinearGradientMode]::Vertical)
	$Graphics.FillRectangle($brush, $rectangle)
	$brush.Dispose()
}

function Draw-NeonText(
	[System.Drawing.Graphics]$Graphics,
	[string]$Text,
	[string]$FontFamily,
	[float]$Size,
	[float]$CenterX,
	[float]$Top,
	[float]$MaximumWidth,
	[System.Drawing.Color]$Fill,
	[System.Drawing.Color]$Neon,
	[float]$OutlineWidth
) {
	$family = [System.Drawing.FontFamily]::new($FontFamily)
	$path = [System.Drawing.Drawing2D.GraphicsPath]::new()
	$origin = [System.Drawing.PointF]::new(0.0, 0.0)
	$format = [System.Drawing.StringFormat]::GenericTypographic
	$path.AddString(
		$Text,
		$family,
		[int][System.Drawing.FontStyle]::Regular,
		$Size,
		$origin,
		$format)

	$bounds = $path.GetBounds()
	if ($bounds.Width -gt $MaximumWidth) {
		$scale = $MaximumWidth / $bounds.Width
		$scaleMatrix = [System.Drawing.Drawing2D.Matrix]::new()
		$scaleMatrix.Scale($scale, $scale)
		$path.Transform($scaleMatrix)
		$scaleMatrix.Dispose()
		$bounds = $path.GetBounds()
	}

	$move = [System.Drawing.Drawing2D.Matrix]::new()
	$move.Translate(
		$CenterX - ($bounds.X + $bounds.Width / 2.0),
		$Top - $bounds.Y)
	$path.Transform($move)
	$move.Dispose()

	$glowOuter = [System.Drawing.Pen]::new(
		[System.Drawing.Color]::FromArgb(55, $Neon.R, $Neon.G, $Neon.B),
		$OutlineWidth * 6.0)
	$glowInner = [System.Drawing.Pen]::new(
		[System.Drawing.Color]::FromArgb(115, $Neon.R, $Neon.G, $Neon.B),
		$OutlineWidth * 3.0)
	$darkEdge = [System.Drawing.Pen]::new(
		[System.Drawing.Color]::FromArgb(235, 8, 4, 18),
		$OutlineWidth * 1.8)
	$neonEdge = [System.Drawing.Pen]::new($Neon, $OutlineWidth)
	foreach ($pen in @($glowOuter, $glowInner, $darkEdge, $neonEdge)) {
		$pen.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round
		$Graphics.DrawPath($pen, $path)
	}

	$fillBrush = [System.Drawing.SolidBrush]::new($Fill)
	$Graphics.FillPath($fillBrush, $path)

	$fillBrush.Dispose()
	$glowOuter.Dispose()
	$glowInner.Dispose()
	$darkEdge.Dispose()
	$neonEdge.Dispose()
	$path.Dispose()
	$family.Dispose()
}

function Draw-Subtitle(
	[System.Drawing.Graphics]$Graphics,
	[string]$Text,
	[float]$Size,
	[float]$CenterX,
	[float]$Top,
	[System.Drawing.Color]$Colour
) {
	$font = [System.Drawing.Font]::new(
		"Bahnschrift",
		$Size,
		[System.Drawing.FontStyle]::Bold,
		[System.Drawing.GraphicsUnit]::Pixel)
	$format = [System.Drawing.StringFormat]::new()
	$format.Alignment = [System.Drawing.StringAlignment]::Center
	$format.LineAlignment = [System.Drawing.StringAlignment]::Near
	$shadow = [System.Drawing.SolidBrush]::new(
		[System.Drawing.Color]::FromArgb(220, 2, 2, 8))
	$brush = [System.Drawing.SolidBrush]::new($Colour)
	$Graphics.DrawString(
		$Text,
		$font,
		$shadow,
		[System.Drawing.PointF]::new($CenterX + 2.0, $Top + 2.0),
		$format)
	$Graphics.DrawString(
		$Text,
		$font,
		$brush,
		[System.Drawing.PointF]::new($CenterX, $Top),
		$format)
	$brush.Dispose()
	$shadow.Dispose()
	$format.Dispose()
	$font.Dispose()
}

New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null

$listingInput = [System.Drawing.Image]::FromFile($ListingSource)
$listing = New-Canvas 580 470
$listingGraphics = New-Graphics $listing
Draw-CoverCrop $listingGraphics $listingInput 580 470 0.46
Add-TopShade $listingGraphics 580 470 180 120
Draw-NeonText `
	$listingGraphics `
	"VICE CITY VR" `
	"Impact" `
	68 `
	290 `
	18 `
	520 `
	([System.Drawing.Color]::White) `
	([System.Drawing.Color]::FromArgb(255, 255, 43, 174)) `
	2.2
Draw-Subtitle `
	$listingGraphics `
	"NATIVE QUEST VR MOD" `
	18 `
	290 `
	96 `
	([System.Drawing.Color]::FromArgb(255, 73, 235, 255))
$listingPath = Join-Path $OutputDirectory "sidequest-listing-580x470.png"
$listing.Save($listingPath, [System.Drawing.Imaging.ImageFormat]::Png)
$listingGraphics.Dispose()
$listing.Dispose()
$listingInput.Dispose()

$bannerInput = [System.Drawing.Image]::FromFile($BannerSource)

$bannerClean = New-Canvas 2048 370
$bannerCleanGraphics = New-Graphics $bannerClean
Draw-CoverCrop $bannerCleanGraphics $bannerInput 2048 370 0.36
$bannerCleanPath =
	Join-Path $OutputDirectory "sidequest-banner-clean-2048x370.png"
$bannerClean.Save($bannerCleanPath, [System.Drawing.Imaging.ImageFormat]::Png)
$bannerCleanGraphics.Dispose()
$bannerClean.Dispose()

$banner = New-Canvas 2048 370
$bannerGraphics = New-Graphics $banner
Draw-CoverCrop $bannerGraphics $bannerInput 2048 370 0.36

$titleBackdrop = [System.Drawing.Rectangle]::new(650, 28, 748, 300)
$backdropPath = [System.Drawing.Drawing2D.GraphicsPath]::new()
$radius = 36
$backdropPath.AddArc(
	$titleBackdrop.Left, $titleBackdrop.Top, $radius, $radius, 180, 90)
$backdropPath.AddArc(
	$titleBackdrop.Right - $radius, $titleBackdrop.Top,
	$radius, $radius, 270, 90)
$backdropPath.AddArc(
	$titleBackdrop.Right - $radius, $titleBackdrop.Bottom - $radius,
	$radius, $radius, 0, 90)
$backdropPath.AddArc(
	$titleBackdrop.Left, $titleBackdrop.Bottom - $radius,
	$radius, $radius, 90, 90)
$backdropPath.CloseFigure()
$backdropBrush = [System.Drawing.SolidBrush]::new(
	[System.Drawing.Color]::FromArgb(105, 2, 1, 10))
$bannerGraphics.FillPath($backdropBrush, $backdropPath)
$backdropBrush.Dispose()
$backdropPath.Dispose()

Draw-NeonText `
	$bannerGraphics `
	"VICE CITY VR" `
	"Impact" `
	116 `
	1024 `
	72 `
	520 `
	([System.Drawing.Color]::White) `
	([System.Drawing.Color]::FromArgb(255, 255, 43, 174)) `
	3.4
Draw-Subtitle `
	$bannerGraphics `
	"NATIVE QUEST VR MOD - v0.3.1 ALPHA" `
	26 `
	1024 `
	215 `
	([System.Drawing.Color]::FromArgb(255, 73, 235, 255))

$bannerPath = Join-Path $OutputDirectory "sidequest-banner-2048x370.png"
$banner.Save($bannerPath, [System.Drawing.Imaging.ImageFormat]::Png)
$bannerGraphics.Dispose()
$banner.Dispose()
$bannerInput.Dispose()

Write-Output $listingPath
Write-Output $bannerPath
Write-Output $bannerCleanPath
