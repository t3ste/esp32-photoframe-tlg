#!/usr/bin/env node

//# Mit Portrait Combine + Thumbnails
//node batch-convert.js C:\Photos\Input C:\Photos\Output

//# Ohne Portrait Combine + Thumbnails
//node batch-convert.js C:\Photos\Input C:\Photos\Output --no-combine


import { createCanvas, loadImage } from "canvas";
import fs from "fs";
import path from "path";
import { fileURLToPath } from "url";
import { processImage, PALETTE_MEASURED } from "./image-processor.js";

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

const DISPLAY_WIDTH = 800;
const DISPLAY_HEIGHT = 480;

// Thumbnail dimensions (matching ESP32 web interface)
const THUMB_WIDTH_LANDSCAPE = 160;
const THUMB_HEIGHT_LANDSCAPE = 96;
const THUMB_WIDTH_PORTRAIT = 96;
const THUMB_HEIGHT_PORTRAIT = 160;

// Default processing parameters (matching ESP32 defaults)
const DEFAULT_PARAMS = {
  exposure: 1.0,
  saturation: 1.3,
  toneMode: "scurve",
  contrast: 1.0,
  strength: 0.9,
  shadowBoost: 0.0,
  highlightCompress: 1.5,
  midpoint: 0.5,
  colorMethod: "rgb",
  renderMeasured: false,
  processingMode: "enhanced",
};

// ===== BMP Writer (24-bit RGB) =====
function writeBMP(imageData, outputPath) {
  const width = imageData.width;
  const height = imageData.height;
  const data = imageData.data;

  const fileHeaderSize = 14;
  const infoHeaderSize = 40;
  const headerSize = fileHeaderSize + infoHeaderSize;
  const rowSize = Math.floor((width * 3 + 3) / 4) * 4;
  const imageSize = rowSize * height;
  const fileSize = headerSize + imageSize;

  const buffer = Buffer.alloc(fileSize);
  let offset = 0;

  // File header
  buffer.write("BM", offset); offset += 2;
  buffer.writeUInt32LE(fileSize, offset); offset += 4;
  buffer.writeUInt32LE(0, offset); offset += 4;
  buffer.writeUInt32LE(headerSize, offset); offset += 4;

  // Info header
  buffer.writeUInt32LE(infoHeaderSize, offset); offset += 4;
  buffer.writeInt32LE(width, offset); offset += 4;
  buffer.writeInt32LE(height, offset); offset += 4;
  buffer.writeUInt16LE(1, offset); offset += 2;
  buffer.writeUInt16LE(24, offset); offset += 2;
  buffer.writeUInt32LE(0, offset); offset += 4;
  buffer.writeUInt32LE(imageSize, offset); offset += 4;
  buffer.writeInt32LE(2835, offset); offset += 4;
  buffer.writeInt32LE(2835, offset); offset += 4;
  buffer.writeUInt32LE(0, offset); offset += 4;
  buffer.writeUInt32LE(0, offset); offset += 4;

  // Pixel data (bottom-up, BGR)
  for (let y = height - 1; y >= 0; y--) {
    for (let x = 0; x < width; x++) {
      const idx = (y * width + x) * 4;
      buffer.writeUInt8(data[idx + 2], offset++); // B
      buffer.writeUInt8(data[idx + 1], offset++); // G
      buffer.writeUInt8(data[idx], offset++); // R
    }
    const padding = rowSize - width * 3;
    for (let i = 0; i < padding; i++) {
      buffer.writeUInt8(0, offset++);
    }
  }

  fs.writeFileSync(outputPath, buffer);
}

// ===== Thumbnail Generator =====
function generateThumbnail(originalImage, isPortrait, outputPath) {
  const thumbWidth = isPortrait ? THUMB_WIDTH_PORTRAIT : THUMB_WIDTH_LANDSCAPE;
  const thumbHeight = isPortrait ? THUMB_HEIGHT_PORTRAIT : THUMB_HEIGHT_LANDSCAPE;

  const thumbCanvas = createCanvas(thumbWidth, thumbHeight);
  const thumbCtx = thumbCanvas.getContext("2d");

  const srcWidth = originalImage.width;
  const srcHeight = originalImage.height;
  const scaleX = thumbWidth / srcWidth;
  const scaleY = thumbHeight / srcHeight;
  const scale = Math.max(scaleX, scaleY);

  const scaledWidth = Math.round(srcWidth * scale);
  const scaledHeight = Math.round(srcHeight * scale);
  const cropX = Math.round((scaledWidth - thumbWidth) / 2);
  const cropY = Math.round((scaledHeight - thumbHeight) / 2);

  thumbCtx.drawImage(
    originalImage,
    cropX / scale, cropY / scale,
    thumbWidth / scale, thumbHeight / scale,
    0, 0,
    thumbWidth, thumbHeight
  );

  const buffer = thumbCanvas.toBuffer("image/jpeg", { quality: 0.8 });
  fs.writeFileSync(outputPath, buffer);
}

// ===== Image Resizing (Cover Mode) =====
function resizeImageCover(canvas, targetWidth, targetHeight) {
  const srcWidth = canvas.width;
  const srcHeight = canvas.height;

  const scaleX = targetWidth / srcWidth;
  const scaleY = targetHeight / srcHeight;
  const scale = Math.max(scaleX, scaleY);

  const scaledWidth = Math.round(srcWidth * scale);
  const scaledHeight = Math.round(srcHeight * scale);

  const tempCanvas = createCanvas(scaledWidth, scaledHeight);
  const tempCtx = tempCanvas.getContext("2d");
  tempCtx.drawImage(canvas, 0, 0, scaledWidth, scaledHeight);

  const cropX = Math.round((scaledWidth - targetWidth) / 2);
  const cropY = Math.round((scaledHeight - targetHeight) / 2);

  const outputCanvas = createCanvas(targetWidth, targetHeight);
  const outputCtx = outputCanvas.getContext("2d");
  outputCtx.drawImage(
    tempCanvas,
    cropX, cropY, targetWidth, targetHeight,
    0, 0, targetWidth, targetHeight
  );

  return outputCanvas;
}

// ===== 90° Rotation =====
function rotate90Clockwise(canvas) {
  const rotatedCanvas = createCanvas(canvas.height, canvas.width);
  const ctx = rotatedCanvas.getContext("2d");
  ctx.translate(canvas.height, 0);
  ctx.rotate(Math.PI / 2);
  ctx.drawImage(canvas, 0, 0);
  return rotatedCanvas;
}

// ===== Portrait Combine (mit Dithering NACH Kombination) =====
async function combinePortraits(portraitPath1, portraitPath2, outputBmpPath, outputThumbPath) {
  console.log(`  📐 Combining portraits:`);
  console.log(`     Left:  ${path.basename(portraitPath1)}`);
  console.log(`     Right: ${path.basename(portraitPath2)}`);

  // Load both portraits (RGB BMPs without dithering)
  const img1 = await loadImage(portraitPath1);
  const img2 = await loadImage(portraitPath2);

  // Create combined canvas (800x480)
  const combinedCanvas = createCanvas(DISPLAY_WIDTH, DISPLAY_HEIGHT);
  const ctx = combinedCanvas.getContext("2d");

  // Place side-by-side (each 400x480)
  ctx.drawImage(img1, 0, 0, 400, 480);
  ctx.drawImage(img2, 400, 0, 400, 480);

  // ✅ Generate thumbnail BEFORE dithering (better quality)
  const combinedImg = await loadImage(combinedCanvas.toBuffer("image/png"));
  generateThumbnail(combinedImg, false, outputThumbPath);

  // ✅ NOW apply dithering to combined image
  const imageData = ctx.getImageData(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT);
  processImage(imageData, DEFAULT_PARAMS);
  ctx.putImageData(imageData, 0, 0);

  // Write final dithered BMP
  writeBMP(imageData, outputBmpPath);
  console.log(`  ✅ Combined BMP: ${path.basename(outputBmpPath)}`);
  console.log(`  🖼️  Thumbnail: ${path.basename(outputThumbPath)} (${THUMB_WIDTH_LANDSCAPE}x${THUMB_HEIGHT_LANDSCAPE})`);
}

// ===== Process Portrait (NO Dithering, RGB only) =====
async function processPortrait_NoDithering(inputPath, outputBmpPath, outputThumbPath) {
  const img = await loadImage(inputPath);
  let canvas = createCanvas(img.width, img.height);
  let ctx = canvas.getContext("2d");
  ctx.drawImage(img, 0, 0);

  // Generate thumbnail from original
  generateThumbnail(img, true, outputThumbPath);
  console.log(`  🖼️  Thumbnail: ${path.basename(outputThumbPath)} (${THUMB_WIDTH_PORTRAIT}x${THUMB_HEIGHT_PORTRAIT})`);

  // Resize to 400x480 (half width for later combination)
  const targetWidth = 400;
  const targetHeight = DISPLAY_HEIGHT;
  canvas = resizeImageCover(canvas, targetWidth, targetHeight);

  // ✅ Save as RGB BMP WITHOUT dithering (for later combination)
  ctx = canvas.getContext("2d");
  const imageData = ctx.getImageData(0, 0, canvas.width, canvas.height);
  writeBMP(imageData, outputBmpPath); // No processImage() call = no dithering

  console.log(`  🖼️  Portrait mode (400x480 RGB, no dithering yet)`);
}

// ===== Process Landscape/Rotated Portrait (WITH Dithering) =====
async function processLandscape_WithDithering(inputPath, outputBmpPath, outputThumbPath, isPortrait) {
  const img = await loadImage(inputPath);
  let canvas = createCanvas(img.width, img.height);
  let ctx = canvas.getContext("2d");
  ctx.drawImage(img, 0, 0);

  // Generate thumbnail from original
  generateThumbnail(img, false, outputThumbPath);
  console.log(`  🖼️  Thumbnail: ${path.basename(outputThumbPath)} (${THUMB_WIDTH_LANDSCAPE}x${THUMB_HEIGHT_LANDSCAPE})`);

  // Rotate if portrait
  if (isPortrait) {
    console.log(`  🔄 Rotating portrait 90° → landscape`);
    canvas = rotate90Clockwise(canvas);
  }

  // Resize to 800x480
  canvas = resizeImageCover(canvas, DISPLAY_WIDTH, DISPLAY_HEIGHT);

  // ✅ Apply dithering
  ctx = canvas.getContext("2d");
  const imageData = ctx.getImageData(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT);
  processImage(imageData, DEFAULT_PARAMS);
  ctx.putImageData(imageData, 0, 0);

  writeBMP(imageData, outputBmpPath);
  console.log(`  ✅ BMP: ${path.basename(outputBmpPath)}`);
}

// ===== Main Batch Processor =====
async function batchConvert(inputDir, outputDir, portraitCombineEnabled = true) {
  console.log("╔══════════════════════════════════════════════════════════╗");
  console.log("║     ESP32 PhotoFrame Batch Converter (Windows)          ║");
  console.log("╚══════════════════════════════════════════════════════════╝\n");

  console.log(`📂 Input:  ${inputDir}`);
  console.log(`📂 Output: ${outputDir}`);
  console.log(`🖼️  Portrait Combine: ${portraitCombineEnabled ? "ENABLED" : "DISABLED"}`);
  console.log(`📸 Thumbnails: ENABLED (160x96 / 96x160, JPEG Q80%)`);
  console.log(`🎨 Dithering: Applied AFTER portrait combination\n`);

  // Create output directory
  if (!fs.existsSync(outputDir)) {
    fs.mkdirSync(outputDir, { recursive: true });
  }

  // ✅ Portrait directory for temporary RGB BMPs (no dithering)
  const portraitDir = path.join(outputDir, "portrait_temp");
  if (portraitCombineEnabled && !fs.existsSync(portraitDir)) {
    fs.mkdirSync(portraitDir, { recursive: true });
  }

  // Get all JPG files
  const files = fs.readdirSync(inputDir);
  const imageFiles = files.filter((f) => {
    const ext = path.extname(f).toLowerCase();
    return [".jpg", ".jpeg"].includes(ext);
  });

  if (imageFiles.length === 0) {
    console.log("❌ No JPG images found in input directory!");
    return;
  }

  console.log(`📊 Found ${imageFiles.length} image(s)\n`);

  let processed = 0;
  let portraits = [];

  // ===== Phase 1: Process all images =====
  for (let i = 0; i < imageFiles.length; i++) {
    const imageFile = imageFiles[i];
    const inputPath = path.join(inputDir, imageFile);
    const baseName = path.basename(imageFile, path.extname(imageFile));

    console.log(`[${i + 1}/${imageFiles.length}] ${imageFile}`);

    try {
      const img = await loadImage(inputPath);
      const isPortrait = img.height > img.width;

      if (isPortrait && portraitCombineEnabled) {
        // ✅ Save as RGB BMP (NO dithering) for later combination
        const portraitBmp = path.join(portraitDir, `${baseName}.bmp`);
        const portraitThumb = path.join(outputDir, `${baseName}.jpg`); // ✅ Thumbnail in Output
        await processPortrait_NoDithering(inputPath, portraitBmp, portraitThumb);
        portraits.push({ bmp: portraitBmp, basename: baseName });
        console.log(`  💾 Saved to portrait queue (${portraits.length} total)`);
      } else {
        // ✅ Landscape: dither immediately, save to Output
        const outputBmp = path.join(outputDir, `${baseName}.bmp`);
        const outputThumb = path.join(outputDir, `${baseName}.jpg`);
        await processLandscape_WithDithering(inputPath, outputBmp, outputThumb, isPortrait);
        processed++;
      }
    } catch (error) {
      console.error(`  ❌ ERROR: ${error.message}`);
    }
  }

  // ===== Phase 2: Combine portraits =====
  if (portraitCombineEnabled && portraits.length > 0) {
    console.log(`\n🔀 Combining ${portraits.length} portrait(s)...\n`);
    
    for (let i = 0; i < portraits.length; i += 2) {
      if (i + 1 < portraits.length) {
        // ✅ Pair found: Combine and apply dithering
        const combinedName = `combined_${Math.floor(i / 2) + 1}`;
        const combinedBmp = path.join(outputDir, `${combinedName}.bmp`); // ✅ Direct to Output
        const combinedThumb = path.join(outputDir, `${combinedName}.jpg`);
        await combinePortraits(
          portraits[i].bmp, 
          portraits[i + 1].bmp, 
          combinedBmp,
          combinedThumb
        );
        processed++;
      } else {
        // ✅ Unpaired portrait: Rotate and dither
        console.log(`  ⚠️  Unpaired portrait: ${portraits[i].basename}`);
        console.log(`     Converting with rotation to landscape...`);
        
        const img = await loadImage(portraits[i].bmp);
        const outputBmp = path.join(outputDir, `${portraits[i].basename}_rotated.bmp`);
        const outputThumb = path.join(outputDir, `${portraits[i].basename}_rotated.jpg`);
        
        let canvas = createCanvas(img.width, img.height);
        let ctx = canvas.getContext("2d");
        ctx.drawImage(img, 0, 0);
        
        // Generate thumbnail
        const rotatedCanvas = rotate90Clockwise(canvas);
        const rotatedImg = await loadImage(rotatedCanvas.toBuffer("image/png"));
        generateThumbnail(rotatedImg, false, outputThumb);
        
        canvas = rotate90Clockwise(canvas);
        canvas = resizeImageCover(canvas, DISPLAY_WIDTH, DISPLAY_HEIGHT);
        
        // ✅ Apply dithering
        ctx = canvas.getContext("2d");
        const imageData = ctx.getImageData(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT);
        processImage(imageData, DEFAULT_PARAMS);
        ctx.putImageData(imageData, 0, 0);
        writeBMP(imageData, outputBmp);
        
        console.log(`     ✅ BMP: ${path.basename(outputBmp)}`);
        console.log(`     🖼️  Thumbnail: ${path.basename(outputThumb)}`);
        processed++;
      }
    }

    // ✅ Clean up temporary portrait directory
    console.log(`\n🧹 Cleaning up temporary portrait directory...`);
    fs.rmSync(portraitDir, { recursive: true, force: true });
  }

  console.log("\n═══════════════════════════════════════════════════════════");
  console.log(`✅ Conversion complete!`);
  console.log(`   Processed: ${processed} BMP(s) + Thumbnails`);
  console.log(`   Output:    ${outputDir}`);
  console.log("═══════════════════════════════════════════════════════════\n");
}

// ===== CLI Entry Point =====
const args = process.argv.slice(2);

if (args.length < 2) {
  console.log("Usage: node batch-convert.js <input_folder> <output_folder> [--no-combine]");
  console.log("\nOptions:");
  console.log("  --no-combine    Disable portrait combine (rotate portraits individually)");
  console.log("\nFeatures:");
  console.log("  ✅ ESP32-compatible BMP output (800x480, 7-color dithered)");
  console.log("  ✅ Portrait combine (2 portraits → 1 landscape)");
  console.log("  ✅ Dithering AFTER combination (better quality)");
  console.log("  ✅ Thumbnail generation (160x96 landscape, 96x160 portrait, JPEG 80%)");
  console.log("\nExample:");
  console.log("  node batch-convert.js ./input ./output");
  console.log("  node batch-convert.js C:\\Photos C:\\Output --no-combine");
  process.exit(1);
}

const inputDir = args[0];
const outputDir = args[1];
const portraitCombineEnabled = !args.includes("--no-combine");

if (!fs.existsSync(inputDir)) {
  console.error(`❌ Input directory does not exist: ${inputDir}`);
  process.exit(1);
}

batchConvert(inputDir, outputDir, portraitCombineEnabled).catch((err) => {
  console.error(`❌ Fatal error: ${err.message}`);
  process.exit(1);
});
