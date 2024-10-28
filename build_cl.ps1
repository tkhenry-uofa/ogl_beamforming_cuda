# Set initial flags
$cflags = "/arch:AVX /W3 /I ./external/include"
$ldflags = "/link /LIBPATH:./external/lib libraylib.dll gdi32.lib winmm.lib cuda_toolkit.lib"
$output_path = "../x64"
$app_name = "ogl_beamforming_cuda"

# Check for -d flag (debug mode)
if ($args[0] -eq "-d") {
    $cflags += " /Z7 /Od"   # Debug info, disable optimization
    $ldflags += " /LIBPATH:../x64/Debug"
    $output_path += "/Debug/"
    Write-Output "Building debug OGL"
} else {
    $cflags += " /O2"       # Release optimization
    $ldflags += " /LIBPATH:../x64/Release"
    $output_path += "/Release/"
    Write-Output "Building release OGL"
}

# Create the output directory if it doesn't exist
if (!(Test-Path -Path $output_path)) {
    New-Item -ItemType Directory -Path $output_path | Out-Null
}

# Define the output executable path
$output = "${output_path}${app_name}.exe"

# Run the CL compiler with the specified flags and source file
cl $cflags /Fe:$output main_generic.c $ldflags