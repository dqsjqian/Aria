package com.example.aria.demo5.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.viewmodel.compose.viewModel
import com.example.aria.demo5.viewmodel.MainViewModel
import kotlinx.coroutines.delay

/**
 * Demo5Theme — Material3 theme for the demo
 */
@Composable
fun Demo5Theme(content: @Composable () -> Unit) {
    MaterialTheme(
        colorScheme = dynamicColorScheme(),
        content = content
    )
}

@Composable
private fun dynamicColorScheme(): ColorScheme {
    // Use default Material3 light color scheme
    return lightColorScheme()
}

/**
 * MainScreen — root of the Compose UI tree
 *
 * Mirrors iOS demo3's RootView layout:
 *   Title → Subtitle → Status → Result →
 *   5 pattern buttons (2x2 + 1 full-width) →
 *   Separator → Legend →
 *   Separator → Playground section →
 *   Floating toast (pinned to bottom)
 *
 * Layout strategy (mental-model differences vs the iOS sibling):
 *   - iOS uses Auto Layout (NSLayoutAnchor) with Y down
 *   - Android uses Compose Column/Row with top-to-bottom flow
 *   - safeAreaLayoutGuide → WindowInsets / PaddingValues
 *   - No NSBox needed; the separator is just a 1dp-tall HorizontalDivider
 */
@Composable
fun MainScreen() {
    val viewModel: MainViewModel = viewModel()
    val status by viewModel.statusText.collectAsState()
    val result by viewModel.resultText.collectAsState()
    val toastMessage by viewModel.toastMessage.collectAsState()

    // ── Playground panel state ──────────────────────────────────────────────
    var showPanelA by remember { mutableStateOf(false) }
    var showPanelC by remember { mutableStateOf(false) }
    // B and D are full-screen pushes — replace the main content.
    var activeFullScreen by remember { mutableStateOf<String?>(null) }

    Box(modifier = Modifier.fillMaxSize()) {

        // Panel B / D — full-screen push (replaces main content)
        when (activeFullScreen) {
            "B" -> LayoutPlaygroundPanel(onBack = { activeFullScreen = null })
            "D" -> ScrollPlaygroundPanel(onBack = { activeFullScreen = null })
            else -> {
                // Main scrollable content
                Column(
                    modifier = Modifier
                        .fillMaxSize()
                        .verticalScroll(rememberScrollState())
                        .padding(horizontal = 20.dp)
                        .padding(top = 16.dp)
                        .windowInsetsPadding(WindowInsets.safeDrawing)
                ) {
                    // ── Title ────────────────────────────────────────────────
                    Text(
                        text = "MVVM + aria Demo (Android)",
                        fontSize = 20.sp,
                        fontWeight = FontWeight.Bold,
                        fontFamily = FontFamily.Monospace,
                        color = MaterialTheme.colorScheme.onSurface,
                        modifier = Modifier.fillMaxWidth()
                    )

                    Spacer(modifier = Modifier.height(4.dp))

                    // ── Subtitle ────────────────────────────────────────────
                    Text(
                        text = "Powered by C++ aria framework",
                        fontSize = 12.sp,
                        fontFamily = FontFamily.Monospace,
                        color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.6f),
                        modifier = Modifier.fillMaxWidth()
                    )

                    Spacer(modifier = Modifier.height(12.dp))

                    // ── Status label ─────────────────────────────────────────
                    Text(
                        text = status,
                        style = TextStyle(
                            fontSize = 13.sp,
                            fontFamily = FontFamily.Monospace,
                            color = MaterialTheme.colorScheme.onSurfaceVariant
                        ),
                        modifier = Modifier.fillMaxWidth()
                    )

                    Spacer(modifier = Modifier.height(6.dp))

                    // ── Result label ─────────────────────────────────────────
                    Text(
                        text = result,
                        style = TextStyle(
                            fontSize = 14.sp,
                            fontFamily = FontFamily.Monospace,
                            color = Color(0xFF4CAF50) // Green
                        ),
                        modifier = Modifier.fillMaxWidth()
                    )

                    Spacer(modifier = Modifier.height(16.dp))

                    // ── 5 pattern buttons (2x2 + 1 full-width) ──────────────
                    PatternButtonGrid(viewModel)

                    Spacer(modifier = Modifier.height(18.dp))

                    // ── Separator 1 ─────────────────────────────────────────
                    HorizontalDivider(
                        modifier = Modifier.fillMaxWidth(),
                        thickness = 1.dp,
                        color = MaterialTheme.colorScheme.outlineVariant
                    )

                    Spacer(modifier = Modifier.height(10.dp))

                    // ── Legend ───────────────────────────────────────────────
                    Text(
                        text = "Flow: Compose → JniBridge → C++ Command → Model(2s)\n" +
                                "       → C++ Property::on_changed → JNI → StateFlow → Compose\n" +
                                "All 5 OC patterns unified into:\n" +
                                "  aria::Property<string>  (replaces KVO/Block/Notification/Delegate)\n" +
                                "  aria::Command<string>  (replaces Target-Action)",
                        fontSize = 11.sp,
                        fontFamily = FontFamily.Monospace,
                        color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.6f),
                        modifier = Modifier.fillMaxWidth()
                    )

                    Spacer(modifier = Modifier.height(18.dp))

                    // ── Separator 2 ─────────────────────────────────────────
                    HorizontalDivider(
                        modifier = Modifier.fillMaxWidth(),
                        thickness = 1.dp,
                        color = MaterialTheme.colorScheme.outlineVariant
                    )

                    Spacer(modifier = Modifier.height(12.dp))

                    // ── Playground section ──────────────────────────────────
                    Text(
                        text = "🧪 Playground — 空白面板随你发挥",
                        fontSize = 14.sp,
                        fontWeight = FontWeight.Bold,
                        fontFamily = FontFamily.Monospace,
                        color = MaterialTheme.colorScheme.onSurface,
                        modifier = Modifier.fillMaxWidth()
                    )

                    Spacer(modifier = Modifier.height(4.dp))

                    Text(
                        text = "点击按钮会弹出对应面板，可在里面自由添加 UI / C++ VM / 绑定等实验代码。",
                        fontSize = 11.sp,
                        fontFamily = FontFamily.Monospace,
                        color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.6f),
                        modifier = Modifier.fillMaxWidth()
                    )

                    Spacer(modifier = Modifier.height(12.dp))

                    // ── Playground buttons (2x2 grid) ────────────────────────
                    PlaygroundButtonGrid(
                        onPanelA = { showPanelA = true },
                        onPanelB = { activeFullScreen = "B" },
                        onPanelC = { showPanelC = true },
                        onPanelD = { activeFullScreen = "D" }
                    )

                    // Bottom padding for toast
                    Spacer(modifier = Modifier.height(60.dp))
                }
            }
        }

        // ── Floating toast (pinned to bottom of safe area) ──────────────────
        if (toastMessage.isNotEmpty() && activeFullScreen == null) {
            ToastLabel(
                message = toastMessage,
                onDismiss = { viewModel.dismissToast() },
                modifier = Modifier
                    .align(Alignment.BottomCenter)
                    .padding(horizontal = 20.dp, vertical = 16.dp)
            )
        }
    }

    // ── Panel A: Dialog (iOS formSheet → Compose AlertDialog) ──────────────
    if (showPanelA) {
        FreePlaygroundPanel(onDismiss = { showPanelA = false })
    }

    // ── Panel C: BottomSheet (iOS pageSheet → Compose ModalBottomSheet) ────
    if (showPanelC) {
        ListPlaygroundSheet(onDismiss = { showPanelC = false })
    }
}

/**
 * PatternButtonGrid — 5 pattern buttons in 2x2 + 1 full-width layout
 *
 * Mirrors iOS demo3's buildPatternButtons:
 *   Row 1: Block | Notification
 *   Row 2: KVO | Delegate
 *   Row 3: TargetAction (full-width)
 */
@Composable
fun PatternButtonGrid(viewModel: MainViewModel) {
    val buttonHeight = 40.dp
    val gap = 8.dp

    // Row 1: Block | Notification
    Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.spacedBy(gap)
    ) {
        PatternButton(
            text = "Block",
            onClick = { viewModel.fetchViaBlock() },
            modifier = Modifier.weight(1f).height(buttonHeight)
        )
        PatternButton(
            text = "Notification",
            onClick = { viewModel.fetchViaNotification() },
            modifier = Modifier.weight(1f).height(buttonHeight)
        )
    }

    Spacer(modifier = Modifier.height(gap))

    // Row 2: KVO | Delegate
    Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.spacedBy(gap)
    ) {
        PatternButton(
            text = "KVO",
            onClick = { viewModel.fetchViaKVO() },
            modifier = Modifier.weight(1f).height(buttonHeight)
        )
        PatternButton(
            text = "Delegate",
            onClick = { viewModel.fetchViaDelegate() },
            modifier = Modifier.weight(1f).height(buttonHeight)
        )
    }

    Spacer(modifier = Modifier.height(gap))

    // Row 3: TargetAction (full-width)
    PatternButton(
        text = "Target-Action",
        onClick = { viewModel.fetchViaTargetAction() },
        modifier = Modifier.fillMaxWidth().height(buttonHeight)
    )
}

/**
 * PatternButton — styled button matching iOS demo3's roundedButtonWithTitle
 *
 * Mimics the macOS bezel rounded-button look:
 *   - Rounded corners (8dp)
 *   - Secondary background color
 *   - Border (0.5dp)
 *   - Monospace font
 */
@Composable
fun PatternButton(
    text: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier
) {
    OutlinedButton(
        onClick = onClick,
        modifier = modifier,
        shape = RoundedCornerShape(8.dp),
        colors = ButtonDefaults.outlinedButtonColors(
            containerColor = MaterialTheme.colorScheme.surfaceVariant,
            contentColor = MaterialTheme.colorScheme.onSurface,
            disabledContainerColor = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.5f),
            disabledContentColor = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.38f)
        ),
        border = ButtonDefaults.outlinedButtonBorder.copy(
            width = 0.5.dp,
            brush = androidx.compose.ui.graphics.SolidColor(
                MaterialTheme.colorScheme.outlineVariant
            )
        )
    ) {
        Text(
            text = text,
            fontSize = 13.sp,
            fontWeight = FontWeight.Medium,
            fontFamily = FontFamily.Monospace
        )
    }
}

/**
 * PlaygroundButtonGrid — 4 playground buttons in 2x2 layout
 *
 * Mirrors iOS demo3's buildPlaygroundButtons:
 *   Row 1: Panel A | Panel B
 *   Row 2: Panel C | Panel D
 */
@Composable
fun PlaygroundButtonGrid(
    onPanelA: () -> Unit,
    onPanelB: () -> Unit,
    onPanelC: () -> Unit,
    onPanelD: () -> Unit
) {
    val buttonHeight = 40.dp
    val gap = 8.dp

    // Row 1: Panel A | Panel B
    Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.spacedBy(gap)
    ) {
        PatternButton(
            text = "Panel A",
            onClick = onPanelA,
            modifier = Modifier.weight(1f).height(buttonHeight)
        )
        PatternButton(
            text = "Panel B",
            onClick = onPanelB,
            modifier = Modifier.weight(1f).height(buttonHeight)
        )
    }

    Spacer(modifier = Modifier.height(gap))

    // Row 2: Panel C | Panel D
    Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.spacedBy(gap)
    ) {
        PatternButton(
            text = "Panel C",
            onClick = onPanelC,
            modifier = Modifier.weight(1f).height(buttonHeight)
        )
        PatternButton(
            text = "Panel D",
            onClick = onPanelD,
            modifier = Modifier.weight(1f).height(buttonHeight)
        )
    }
}

/**
 * ToastLabel — floating toast near the bottom of safe area
 *
 * Mirrors iOS demo3's buildToastLabel:
 *   - Dark background (0.12, 0.12, 0.12, 0.92)
 *   - White text
 *   - Rounded corners (8dp)
 *   - Auto-hides after 4s (fades in/out)
 */
@Composable
fun ToastLabel(
    message: String,
    onDismiss: () -> Unit,
    modifier: Modifier = Modifier
) {
    // Auto-hide after 4 seconds
    LaunchedEffect(message) {
        if (message.isNotEmpty()) {
            delay(4000)
            onDismiss()
        }
    }

    Box(
        modifier = modifier
            .fillMaxWidth()
            .background(
                color = Color(red = 0.12f, green = 0.12f, blue = 0.12f, alpha = 0.92f),
                shape = RoundedCornerShape(8.dp)
            )
            .padding(horizontal = 16.dp, vertical = 12.dp)
    ) {
        Text(
            text = message,
            fontSize = 13.sp,
            fontFamily = FontFamily.Monospace,
            color = Color.White,
            textAlign = TextAlign.Center,
            modifier = Modifier.fillMaxWidth()
        )
    }
}
