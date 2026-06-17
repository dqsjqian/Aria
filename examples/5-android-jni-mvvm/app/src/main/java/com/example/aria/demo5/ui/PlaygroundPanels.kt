package com.example.aria.demo5.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

// ═══════════════════════════════════════════════════════════════════════════════
// Playground panels — Android Compose mirrors of iOS demo3's
//   FreePlaygroundView  / LayoutPlaygroundView / ListPlaygroundView
//   / ScrollPlaygroundView, adapted from UIKit → Compose idioms.
// ═══════════════════════════════════════════════════════════════════════════════

// ── Panel A: Free Playground (iOS → formSheet, Android → Dialog) ─────────────
// A simple text editing area — the iOS twin is a UITextView full-height in
// safe area. Compose equivalent: a Dialog with OutlinedTextField.

@Composable
fun FreePlaygroundPanel(onDismiss: () -> Unit) {
    var text by remember {
        mutableStateOf(
            "// Panel A — Free Playground (Kotlin + Compose)\n" +
            "//\n" +
            "// 这是【A 面板】的 Compose 版本。\n" +
            "// 一个多行编辑框铺满整个弹窗。\n" +
            "//\n" +
            "// 你可以把 Aria 的 C++ ViewModel 接进来：\n" +
            "//   - Property ↔ Compose StateFlow 双向绑定\n" +
            "//   - Command 挂给按钮触发\n"
        )
    }

    AlertDialog(
        onDismissRequest = onDismiss,
        title = {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Text("Panel A — Free Playground", fontSize = 16.sp, fontWeight = FontWeight.Bold)
                TextButton(onClick = { text = "" }) { Text("Clear") }
            }
        },
        text = {
            OutlinedTextField(
                value = text,
                onValueChange = { text = it },
                modifier = Modifier
                    .fillMaxWidth()
                    .heightIn(min = 200.dp, max = 380.dp),
                textStyle = LocalTextStyle.current.copy(
                    fontSize = 13.sp,
                    fontFamily = FontFamily.Monospace
                ),
                colors = OutlinedTextFieldDefaults.colors(
                    focusedBorderColor = MaterialTheme.colorScheme.outlineVariant,
                    unfocusedBorderColor = MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.5f)
                )
            )
        },
        confirmButton = {
            TextButton(onClick = onDismiss) { Text("Close") }
        },
        shape = RoundedCornerShape(12.dp)
    )
}

// ── Panel B: Layout Playground (iOS → full-screen push, Android → full screen) ──
// Demonstrates a form-like layout: avatar, name, three styled buttons,
// email/password fields, sign-in button.  The iOS twin is LayoutPlaygroundView
// with Masonry.

@Composable
fun LayoutPlaygroundPanel(onBack: () -> Unit) {
    var email by remember { mutableStateOf("") }
    var password by remember { mutableStateOf("") }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(20.dp)
            .windowInsetsPadding(WindowInsets.safeDrawing)
    ) {
        // Title bar
        Row(
            modifier = Modifier.fillMaxWidth(),
            verticalAlignment = Alignment.CenterVertically
        ) {
            TextButton(onClick = onBack) { Text("← Back") }
            Spacer(Modifier.weight(1f))
            Text("Panel B", fontSize = 18.sp, fontWeight = FontWeight.Bold)
            Spacer(Modifier.weight(1f))
            Spacer(Modifier.width(64.dp)) // balance the Back button
        }

        Spacer(Modifier.height(24.dp))

        // Avatar + name
        Box(
            modifier = Modifier.fillMaxWidth(),
            contentAlignment = Alignment.Center
        ) {
            Column(horizontalAlignment = Alignment.CenterHorizontally) {
                Box(
                    modifier = Modifier
                        .size(72.dp)
                        .clip(CircleShape)
                        .background(MaterialTheme.colorScheme.primary.copy(alpha = 0.2f)),
                    contentAlignment = Alignment.Center
                ) {
                    Text(
                        "👤",
                        fontSize = 32.sp
                    )
                }
                Spacer(Modifier.height(8.dp))
                Text(
                    "Aria User",
                    fontSize = 16.sp,
                    fontWeight = FontWeight.Medium
                )
            }
        }

        Spacer(Modifier.height(16.dp))

        // Three styled buttons — matching iOS's primary/secondary/danger row
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(8.dp)
        ) {
            Button(
                onClick = { /* demo — no-op */ },
                modifier = Modifier.weight(1f),
                shape = RoundedCornerShape(8.dp)
            ) {
                Text("Primary", fontSize = 13.sp)
            }
            OutlinedButton(
                onClick = { /* demo — no-op */ },
                modifier = Modifier.weight(1f),
                shape = RoundedCornerShape(8.dp)
            ) {
                Text("Secondary", fontSize = 13.sp)
            }
        }
        Spacer(Modifier.height(8.dp))
        OutlinedButton(
            onClick = { /* demo — no-op */ },
            modifier = Modifier.fillMaxWidth(),
            shape = RoundedCornerShape(8.dp),
            colors = ButtonDefaults.outlinedButtonColors(
                contentColor = Color(0xFFD32F2F)
            )
        ) {
            Text("Danger", fontSize = 13.sp)
        }

        Spacer(Modifier.height(24.dp))

        // Form fields
        Card(
            modifier = Modifier.fillMaxWidth(),
            shape = RoundedCornerShape(12.dp),
            colors = CardDefaults.cardColors(
                containerColor = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.4f)
            )
        ) {
            Column(modifier = Modifier.padding(16.dp)) {
                Text("Sign In", fontSize = 14.sp, fontWeight = FontWeight.Bold)
                Spacer(Modifier.height(12.dp))

                OutlinedTextField(
                    value = email,
                    onValueChange = { email = it },
                    label = { Text("Email") },
                    singleLine = true,
                    modifier = Modifier.fillMaxWidth(),
                    shape = RoundedCornerShape(8.dp)
                )
                Spacer(Modifier.height(8.dp))
                OutlinedTextField(
                    value = password,
                    onValueChange = { password = it },
                    label = { Text("Password") },
                    singleLine = true,
                    modifier = Modifier.fillMaxWidth(),
                    shape = RoundedCornerShape(8.dp)
                )
                Spacer(Modifier.height(12.dp))
                Button(
                    onClick = { /* demo — no-op */ },
                    modifier = Modifier.fillMaxWidth(),
                    shape = RoundedCornerShape(8.dp)
                ) {
                    Text("Sign In")
                }
            }
        }

        Spacer(Modifier.height(16.dp))

        // Hint
        Text(
            text = "这是【B 面板】— Layout Playground 的 Compose 版本。\n" +
                    "iOS 对应 LayoutPlaygroundView (Masonry)。\n" +
                    "在这里可以实验 Aria 的 Property ↔ Compose 绑定。",
            fontSize = 11.sp,
            fontFamily = FontFamily.Monospace,
            color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.6f)
        )
    }
}

// ── Panel C: List Playground (iOS → pageSheet, Android → BottomSheet) ───────
// A scrolling list with icon + title + subtitle items. The iOS twin is
// ListPlaygroundView with a UITableView + LPGItemCell.

private data class ListItem(val icon: String, val title: String, val subtitle: String)

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ListPlaygroundSheet(onDismiss: () -> Unit) {
    val items = remember {
        listOf(
            ListItem("📧", "Email Settings", "Manage notification preferences"),
            ListItem("🔐", "Security", "Two-factor authentication, passkeys"),
            ListItem("🎨", "Appearance", "Theme, font size, dark mode"),
            ListItem("🌐", "Language", "English / 中文 / 日本語"),
            ListItem("💾", "Data & Storage", "Cache, offline content"),
            ListItem("ℹ️", "About", "Version 1.0 — Powered by aria"),
            ListItem("❓", "Help & Feedback", "FAQs, contact support"),
            ListItem("⚙️", "Advanced", "Developer options, logs"),
        )
    }

    val sheetState = rememberModalBottomSheetState()

    ModalBottomSheet(
        onDismissRequest = onDismiss,
        sheetState = sheetState,
        shape = RoundedCornerShape(topStart = 16.dp, topEnd = 16.dp)
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 16.dp)
        ) {
            Text(
                "Panel C — List Playground",
                fontSize = 16.sp,
                fontWeight = FontWeight.Bold,
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(bottom = 12.dp)
            )

            LazyColumn(
                modifier = Modifier.weight(1f, fill = false),
                contentPadding = PaddingValues(bottom = 24.dp)
            ) {
                itemsIndexed(items) { _, item ->
                    ListItemRow(item)
                    HorizontalDivider(
                        color = MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.3f)
                    )
                }
            }
        }
    }
}

@Composable
private fun ListItemRow(item: ListItem) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clickable { /* demo — no-op */ }
            .padding(vertical = 12.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Text(item.icon, fontSize = 22.sp)
        Spacer(Modifier.width(12.dp))
        Column(modifier = Modifier.weight(1f)) {
            Text(item.title, fontSize = 15.sp, fontWeight = FontWeight.Medium)
            Text(
                item.subtitle,
                fontSize = 12.sp,
                color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.7f)
            )
        }
        Text("›", fontSize = 20.sp, color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.4f))
    }
}

// ── Panel D: Scroll Playground (iOS → full-screen push, Android → full screen) ──
// A scrollable profile card with avatar, name, expandable bio, tag buttons.
// The iOS twin is ScrollPlaygroundView with UIScrollView + Masonry.

@Composable
fun ScrollPlaygroundPanel(onBack: () -> Unit) {
    val tags = remember { listOf("Kotlin", "Compose", "aria", "MVVM", "C++", "JNI", "Android") }
    var bioExpanded by remember { mutableStateOf(false) }
    val bioFull = "Aria is a reactive MVVM++ framework for C++20. " +
            "This is the Android JNI demo — the C++ ViewModel drives the Kotlin " +
            "layer through aria::Property / aria::Command, and the Playground " +
            "panels let you experiment with more advanced bindings."

    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(20.dp)
            .windowInsetsPadding(WindowInsets.safeDrawing)
    ) {
        // Title bar
        Row(
            modifier = Modifier.fillMaxWidth(),
            verticalAlignment = Alignment.CenterVertically
        ) {
            TextButton(onClick = onBack) { Text("← Back") }
            Spacer(Modifier.weight(1f))
            Text("Panel D", fontSize = 18.sp, fontWeight = FontWeight.Bold)
            Spacer(Modifier.weight(1f))
            Spacer(Modifier.width(64.dp))
        }

        Spacer(Modifier.height(20.dp))

        // Profile card
        Card(
            modifier = Modifier.fillMaxWidth(),
            shape = RoundedCornerShape(16.dp),
            colors = CardDefaults.cardColors(
                containerColor = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.5f)
            )
        ) {
            Column(
                modifier = Modifier.padding(20.dp),
                horizontalAlignment = Alignment.CenterHorizontally
            ) {
                Box(
                    modifier = Modifier
                        .size(80.dp)
                        .clip(CircleShape)
                        .background(MaterialTheme.colorScheme.primary.copy(alpha = 0.2f)),
                    contentAlignment = Alignment.Center
                ) {
                    Text("👤", fontSize = 36.sp)
                }

                Spacer(Modifier.height(12.dp))

                Text(
                    "Aria Developer",
                    fontSize = 18.sp,
                    fontWeight = FontWeight.Bold
                )
                Text(
                    "@aria_framework",
                    fontSize = 13.sp,
                    color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.6f)
                )

                Spacer(Modifier.height(12.dp))

                // Bio (expandable)
                Text(
                    text = if (bioExpanded) bioFull else bioFull.take(80) + "...",
                    fontSize = 13.sp,
                    lineHeight = 20.sp,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
                TextButton(onClick = { bioExpanded = !bioExpanded }) {
                    Text(if (bioExpanded) "收起" else "展开更多")
                }

                // Tag row
                Spacer(Modifier.height(4.dp))
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.spacedBy(6.dp)
                ) {
                    tags.forEach { tag ->
                        SuggestionChip(
                            onClick = { /* demo — no-op */ },
                            label = { Text(tag, fontSize = 12.sp) },
                            shape = RoundedCornerShape(16.dp)
                        )
                    }
                }
            }
        }

        Spacer(Modifier.height(20.dp))

        // Toggle section
        var toggleOn by remember { mutableStateOf(false) }
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically
        ) {
            Text("Enable experimental features", fontSize = 14.sp)
            Switch(checked = toggleOn, onCheckedChange = { toggleOn = it })
        }

        Spacer(Modifier.height(16.dp))

        Text(
            text = "这是【D 面板】— Scroll Playground 的 Compose 版本。\n" +
                    "iOS 对应 ScrollPlaygroundView (UIScrollView + Masonry)。\n" +
                    "展示了可滚动内容的 Card、展开/收起、标签行、开关。",
            fontSize = 11.sp,
            fontFamily = FontFamily.Monospace,
            color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.6f)
        )
    }
}
