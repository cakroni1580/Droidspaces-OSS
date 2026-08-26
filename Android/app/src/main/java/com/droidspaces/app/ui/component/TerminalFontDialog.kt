package com.droidspaces.app.ui.component

import android.graphics.Typeface
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.font.Typeface as ComposeTypeface
import androidx.compose.ui.unit.dp
import com.droidspaces.app.R
import com.droidspaces.app.ui.theme.JetBrainsMono
import com.droidspaces.app.util.FilePickerUtils
import com.droidspaces.app.util.FontInfo
import java.io.File
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

// The formats Typeface.createFromFile can load; .ttc loads its first face
private val FONT_EXTENSIONS = setOf("ttf", "otf", "ttc")

/**
 * Terminal font picker: the bundled default plus everything imported into
 * filesDir/fonts/, with an import action at the bottom. Imports and deletes
 * touch the disk immediately; the radio selection only lands when confirmed.
 */
@Composable
fun TerminalFontDialog(
    initialSelection: String,
    onConfirm: (String) -> Unit,
    onDismiss: () -> Unit,
) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()

    // Import order, newest last; name only breaks mtime ties so the order
    // never shifts between visits
    fun listFonts() = FontInfo.fontsDir(context).listFiles()
        ?.filter { it.extension.lowercase() in FONT_EXTENSIONS }
        ?.sortedWith(compareBy({ it.lastModified() }, { it.name.lowercase() }))
        .orEmpty()

    var importedFonts by remember { mutableStateOf(listFonts()) }
    var selected by remember { mutableStateOf(initialSelection) }
    var importError by remember { mutableStateOf<String?>(null) }
    val fontNames = remember(importedFonts) {
        importedFonts.associate { it.name to FontInfo.displayName(it) }
    }
    // Each row previews its own face; a file that no longer loads just
    // falls back to the app's mono, name-only
    val fontFamilies = remember(importedFonts) {
        importedFonts.associate { file ->
            file.name to runCatching {
                FontFamily(ComposeTypeface(Typeface.createFromFile(file)))
            }.getOrNull()
        }
    }
    // A selection pointing at a deleted file behaves as the bundled default
    val effectiveSelection = if (importedFonts.any { it.name == selected }) selected else ""

    val fontPickerLauncher = rememberLauncherForActivityResult(
        // Unfiltered like the tarball picker: font MIME types are unreliable
        // across file managers, the real validation is Typeface.createFromFile
        contract = ActivityResultContracts.GetContent()
    ) { uri ->
        if (uri == null) return@rememberLauncherForActivityResult
        scope.launch {
            importError = null
            val fileName = FilePickerUtils.getFileName(context, uri)
            val safeName = fileName
                ?.takeIf { it.substringAfterLast('.').lowercase() in FONT_EXTENSIONS }
                ?.let { FontInfo.safeFileName(it) }
            if (safeName == null) {
                importError = context.getString(R.string.terminal_font_import_error, fileName ?: uri.toString())
                return@launch
            }
            // ponytail: an existing filename is overwritten, re-importing IS the update path
            val dest = File(FontInfo.fontsDir(context).apply { mkdirs() }, safeName)
            val loaded = withContext(Dispatchers.IO) {
                runCatching {
                    context.contentResolver.openInputStream(uri)?.use { input ->
                        dest.outputStream().use { input.copyTo(it) }
                    } ?: error("unreadable uri")
                    Typeface.createFromFile(dest) // throws if the bytes are not a font
                }.isSuccess
            }
            if (loaded) {
                selected = safeName
                importedFonts = listFonts()
            } else {
                dest.delete()
                importError = context.getString(R.string.terminal_font_import_error, fileName)
            }
        }
    }

    DsDialog(
        onDismiss = onDismiss,
        footer = {
            DialogFooterRow(
                dismissLabel = context.getString(R.string.cancel),
                confirmLabel = context.getString(R.string.ok),
                onDismiss = onDismiss,
                onConfirm = { onConfirm(effectiveSelection) },
            )
        }
    ) {
        Column {
            Text(
                context.getString(R.string.terminal_font_family),
                style = MaterialTheme.typography.titleLarge,
                fontWeight = FontWeight.Bold
            )
            Text(
                context.getString(R.string.terminal_font_description),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.6f),
                modifier = Modifier.padding(top = 2.dp)
            )
        }

        Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
            FontRow(
                label = context.getString(R.string.terminal_font_default),
                fontFamily = JetBrainsMono,
                selected = effectiveSelection.isEmpty(),
                onClick = { selected = "" }
            )
            importedFonts.forEach { file ->
                FontRow(
                    label = fontNames[file.name] ?: file.nameWithoutExtension,
                    fontFamily = fontFamilies[file.name] ?: JetBrainsMono,
                    selected = effectiveSelection == file.name,
                    onClick = { selected = file.name },
                    onDelete = {
                        file.delete()
                        if (selected == file.name) selected = ""
                        importedFonts = listFonts()
                    }
                )
            }
        }

        importError?.let {
            Text(
                text = it,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.error
            )
        }

        // Same add-button look as the env vars dialog
        val addBtnShape = RoundedCornerShape(16.dp)
        Surface(
            modifier = Modifier.fillMaxWidth().clip(addBtnShape).clickable(
                onClick = { fontPickerLauncher.launch("*/*") }
            ),
            shape = addBtnShape,
            color = MaterialTheme.colorScheme.surfaceContainerHigh,
            border = BorderStroke(1.dp, MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.3f)),
            tonalElevation = 0.dp
        ) {
            Row(
                modifier = Modifier.padding(12.dp),
                horizontalArrangement = Arrangement.Center,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Icon(Icons.Default.Add, contentDescription = null, modifier = Modifier.size(16.dp), tint = MaterialTheme.colorScheme.onSurfaceVariant)
                Spacer(modifier = Modifier.width(6.dp))
                Text(context.getString(R.string.terminal_font_import), style = MaterialTheme.typography.labelMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        }
    }
}

// Same row style as the terminal user picker in ContainerTerminalScreen
@Composable
private fun FontRow(
    label: String,
    fontFamily: FontFamily,
    selected: Boolean,
    onClick: () -> Unit,
    onDelete: (() -> Unit)? = null,
) {
    Surface(
        onClick = onClick,
        shape = RoundedCornerShape(16.dp),
        color = if (selected) MaterialTheme.colorScheme.primary.copy(alpha = 0.08f)
                else MaterialTheme.colorScheme.onSurface.copy(alpha = 0.04f),
        border = BorderStroke(
            1.dp,
            if (selected) MaterialTheme.colorScheme.primary.copy(alpha = 0.4f)
            else MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.2f)
        ),
        tonalElevation = 0.dp
    ) {
        Row(
            modifier = Modifier.fillMaxWidth().padding(start = 16.dp, end = 8.dp, top = 4.dp, bottom = 4.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Text(
                text = label,
                // The row's own face is the live preview
                fontFamily = fontFamily,
                style = MaterialTheme.typography.bodyLarge,
                fontWeight = if (selected) FontWeight.SemiBold else FontWeight.Normal,
                color = if (selected) MaterialTheme.colorScheme.primary
                        else MaterialTheme.colorScheme.onSurface,
                modifier = Modifier.weight(1f)
            )
            if (onDelete != null) {
                IconButton(onClick = onDelete, modifier = Modifier.size(36.dp)) {
                    Icon(
                        Icons.Default.Delete,
                        contentDescription = LocalContext.current.getString(R.string.terminal_font_delete),
                        modifier = Modifier.size(18.dp),
                        tint = MaterialTheme.colorScheme.error.copy(alpha = 0.7f)
                    )
                }
            }
            RadioButton(
                selected = selected,
                onClick = onClick,
                colors = RadioButtonDefaults.colors(
                    selectedColor = MaterialTheme.colorScheme.primary,
                    unselectedColor = MaterialTheme.colorScheme.outline.copy(alpha = 0.6f)
                )
            )
        }
    }
}
