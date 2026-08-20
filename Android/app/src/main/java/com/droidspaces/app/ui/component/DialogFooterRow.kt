package com.droidspaces.app.ui.component

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp

/**
 * Shared "Cancel / Confirm" dialog footer: two equal-weight rounded [Surface]
 * buttons. Replaces the identical two-Surface footer that was copy-pasted across
 * the app's dialogs. The confirm button dims and disables via
 * [confirmEnabled]; [cancelBorderAlpha] and [textFontWeight] keep the small
 * per-dialog cosmetic differences.
 */
@Composable
fun DialogFooterRow(
    dismissLabel: String,
    confirmLabel: String,
    onDismiss: () -> Unit,
    onConfirm: () -> Unit,
    modifier: Modifier = Modifier,
    confirmEnabled: Boolean = true,
    cancelBorderAlpha: Float = 0.4f,
    textFontWeight: FontWeight = FontWeight.SemiBold,
    confirmColor: Color = MaterialTheme.colorScheme.primary,
) {
    Surface(
        modifier = modifier.fillMaxWidth(),
        color = MaterialTheme.colorScheme.surfaceContainerHigh,
        shape = RoundedCornerShape(20.dp),
        border = BorderStroke(1.dp, MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.35f)),
        tonalElevation = 0.dp
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(4.dp),
            horizontalArrangement = Arrangement.spacedBy(4.dp)
        ) {
            Surface(
                modifier = Modifier
                    .weight(1f)
                    .clip(RoundedCornerShape(16.dp))
                    .clickable(onClick = onDismiss),
                shape = RoundedCornerShape(16.dp),
                color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.06f),
                border = BorderStroke(1.dp, MaterialTheme.colorScheme.outlineVariant.copy(alpha = cancelBorderAlpha)),
                tonalElevation = 0.dp
            ) {
                Box(modifier = Modifier.padding(14.dp), contentAlignment = Alignment.Center) {
                    Text(dismissLabel, style = MaterialTheme.typography.labelLarge, fontWeight = textFontWeight)
                }
            }

            val confirmBtnColor = if (confirmEnabled) confirmColor.copy(alpha = 0.15f) else MaterialTheme.colorScheme.onSurface.copy(alpha = 0.08f)
            val confirmAccent = if (confirmEnabled) confirmColor else MaterialTheme.colorScheme.outlineVariant
            val confirmTextColor = if (confirmEnabled) confirmColor else MaterialTheme.colorScheme.onSurface.copy(alpha = 0.38f)

            Surface(
                modifier = Modifier
                    .weight(1f)
                    .clip(RoundedCornerShape(16.dp))
                    .clickable(enabled = confirmEnabled, onClick = onConfirm),
                shape = RoundedCornerShape(16.dp),
                color = confirmBtnColor,
                border = BorderStroke(1.dp, confirmAccent.copy(alpha = 0.35f)),
                tonalElevation = 0.dp
            ) {
                Box(modifier = Modifier.padding(14.dp), contentAlignment = Alignment.Center) {
                    Text(
                        confirmLabel,
                        style = MaterialTheme.typography.labelLarge,
                        fontWeight = textFontWeight,
                        color = confirmTextColor
                    )
                }
            }
        }
    }
}
