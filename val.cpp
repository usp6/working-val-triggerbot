/*
 * PixelTrigger Pro Enhanced - Center Screen Color Detection
 * FEATURES:
 * 1. Uses proven click system from original script
 * 2. WASD movement lock
 * 3. Adjustable click delay after detection
 * 4. Clean, fixed UI
 */

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <cstdint>
#include <cmath>
#include <chrono>
#include <random>
#include <thread>
#include <atomic>
#include <string>
#include <sstream>
#include <iomanip>
#include <algorithm>
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "comctl32.lib")

// ===================== CONFIGURATION =====================
struct Config {
    // Detection settings - EXACT SCREEN CENTER
    int centerX = 0;  // Will be set to screen center
    int centerY = 0;  // Will be set to screen center
    int radius = 5;   // Detection radius around center
    int tolerance = 30;
    COLORREF targetColor = RGB(255, 0, 0);
    bool useHSV = false;  // NEW: Use HSV color matching instead of RGB
    bool clickWhenDetected = true;
    bool detectionEnabled = false;  // Toggle state

    // Hotkey settings
    bool toggleHotkeyEnabled = true;
    int toggleHotkey = VK_F2;
    int holdKey = 'N';  // NEW: Key to hold for detection (0 = disabled)

    // Click settings
    int cooldownMs = 100;
    int minDelay = 10;
    int maxDelay = 30;
    int clickDelay = 0;  // Delay after detection before clicking (ms)

    // Overlay settings
    bool showOverlay = true;
    COLORREF overlayColor = RGB(255, 0, 0);
    int overlayThickness = 2;
};

// Global variables
Config g_config;
std::atomic<bool> g_running = true;
HWND g_mainWindow = nullptr;
HWND g_overlayWindow = nullptr;
HDC g_overlayDC = nullptr;
HPEN g_circlePen = nullptr;
HPEN g_crosshairPen = nullptr;
HFONT g_textFont = nullptr;

// Capture thread
std::thread g_captureThread;
std::atomic<bool> g_captureThreadRunning = false;

// Random generator for humanization
std::random_device g_rd;
std::mt19937 g_rng(g_rd());

// ===================== MOVEMENT CHECK =====================
bool IsMovementKeyPressed() {
    // Check for WASD keys (movement in most FPS games)
    if (GetAsyncKeyState('W') & 0x8000) return true;
    if (GetAsyncKeyState('A') & 0x8000) return true;
    if (GetAsyncKeyState('S') & 0x8000) return true;
    if (GetAsyncKeyState('D') & 0x8000) return true;
    
    // Also check arrow keys
    if (GetAsyncKeyState(VK_UP) & 0x8000) return true;
    if (GetAsyncKeyState(VK_LEFT) & 0x8000) return true;
    if (GetAsyncKeyState(VK_DOWN) & 0x8000) return true;
    if (GetAsyncKeyState(VK_RIGHT) & 0x8000) return true;
    
    return false;
}

// ===================== UTILITY FUNCTIONS =====================
bool ColorMatch(COLORREF color1, COLORREF color2, int tolerance) {
    int r1 = GetRValue(color1);
    int g1 = GetGValue(color1);
    int b1 = GetBValue(color1);

    int r2 = GetRValue(color2);
    int g2 = GetGValue(color2);
    int b2 = GetBValue(color2);

    return abs(r1 - r2) <= tolerance &&
           abs(g1 - g2) <= tolerance &&
           abs(b1 - b2) <= tolerance;
}

// ===================== GUARANTEED CLICKING (FROM ORIGINAL) =====================
void PerformGuaranteedClick() {
    // Get screen dimensions
    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    
    // Get current cursor position
    POINT cursorPos;
    GetCursorPos(&cursorPos);
    
    // Save original position
    POINT originalPos = cursorPos;
    
    // Method 1: Direct hardware click at current position
    INPUT inputs[3] = {};
    
    // Optional: Move to exact position (in case cursor drifted)
    inputs[0].type = INPUT_MOUSE;
    inputs[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    inputs[0].mi.dx = cursorPos.x * 65535 / screenWidth;
    inputs[0].mi.dy = cursorPos.y * 65535 / screenHeight;
    
    // Press down
    inputs[1].type = INPUT_MOUSE;
    inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    
    // Release
    inputs[2].type = INPUT_MOUSE;
    inputs[2].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    
    // Send all events at once
    SendInput(3, inputs, sizeof(INPUT));
    
    // Alternative method: Also send to foreground window
    HWND foreground = GetForegroundWindow();
    if (foreground) {
        // Convert to client coordinates
        POINT clientPos = cursorPos;
        ScreenToClient(foreground, &clientPos);
        
        // Send mouse messages directly to window
        SendMessage(foreground, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(clientPos.x, clientPos.y));
        Sleep(20);
        SendMessage(foreground, WM_LBUTTONUP, 0, MAKELPARAM(clientPos.x, clientPos.y));
    }
    
    // Also try the legacy mouse_event
    mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
    Sleep(15);
    mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
}

// ===================== SCREEN CAPTURE =====================
bool CaptureScreenPixel(int x, int y, COLORREF& color) {
    HDC hdc = GetDC(nullptr);
    color = GetPixel(hdc, x, y);
    ReleaseDC(nullptr, hdc);
    return color != CLR_INVALID;
}

// ===================== CAPTURE THREAD =====================
void CaptureThread() {
    auto lastClickTime = std::chrono::steady_clock::now();
    
    // Get screen center
    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    g_config.centerX = screenWidth / 2;
    g_config.centerY = screenHeight / 2;
    
    while (g_captureThreadRunning) {
        if (!g_config.detectionEnabled) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // NEW: Check if hold key is required and pressed
        if (g_config.holdKey != 0 && !(GetAsyncKeyState(g_config.holdKey) & 0x8000)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        // NEW: Check if movement keys are pressed
        if (IsMovementKeyPressed()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        
        // Check area around SCREEN CENTER (not cursor)
        bool colorFound = false;
        
        for (int dx = -g_config.radius; dx <= g_config.radius && !colorFound; dx++) {
            for (int dy = -g_config.radius; dy <= g_config.radius && !colorFound; dy++) {
                // Skip if outside circle
                if (dx*dx + dy*dy > g_config.radius*g_config.radius) continue;
                
                COLORREF capturedColor;
                if (CaptureScreenPixel(g_config.centerX + dx, g_config.centerY + dy, capturedColor)) {
                    if (ColorMatch(capturedColor, g_config.targetColor, g_config.tolerance)) {
                        colorFound = true;
                        break;
                    }
                }
            }
        }
        
        if (colorFound) {
            // Check cooldown
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastClickTime).count();
            
            if (elapsed >= g_config.cooldownMs && g_config.clickWhenDetected) {
                // NEW: Apply configurable click delay after detection
                if (g_config.clickDelay > 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(g_config.clickDelay));
                    
                    // Re-check movement after delay
                    if (IsMovementKeyPressed()) {
                        continue;
                    }
                }
                
                // Humanization delay before click
                std::uniform_int_distribution<int> delayDist(g_config.minDelay, g_config.maxDelay);
                int delay = delayDist(g_rng);
                if (delay > 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                }
                
                PerformGuaranteedClick();
                lastClickTime = now;
                
                // Extra delay after click
                std::this_thread::sleep_for(std::chrono::milliseconds(delayDist(g_rng)));
            }
        }
        
        // Small delay to prevent CPU overload
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// ===================== OVERLAY DRAWING =====================
void DrawOverlay() {
    if (!g_overlayDC || !g_config.showOverlay) return;
    
    // Clear with black (transparent color)
    RECT clientRect;
    GetClientRect(g_overlayWindow, &clientRect);
    HBRUSH blackBrush = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(g_overlayDC, &clientRect, blackBrush);
    DeleteObject(blackBrush);
    
    // Setup drawing tools
    HGDIOBJ oldPen = SelectObject(g_overlayDC, g_circlePen);
    HGDIOBJ oldBrush = SelectObject(g_overlayDC, GetStockObject(NULL_BRUSH));
    
    // Draw detection circle at SCREEN CENTER
    int centerX = clientRect.right / 2;
    int centerY = clientRect.bottom / 2;
    
    // Draw circle
    Ellipse(g_overlayDC, 
            centerX - g_config.radius, 
            centerY - g_config.radius,
            centerX + g_config.radius, 
            centerY + g_config.radius);
    
    // Draw crosshair at center
    SelectObject(g_overlayDC, g_crosshairPen);
    
    // Horizontal line
    MoveToEx(g_overlayDC, centerX - 15, centerY, NULL);
    LineTo(g_overlayDC, centerX + 15, centerY);
    
    // Vertical line
    MoveToEx(g_overlayDC, centerX, centerY - 15, NULL);
    LineTo(g_overlayDC, centerX, centerY + 15);
    
    // Draw center dot
    Ellipse(g_overlayDC, centerX - 2, centerY - 2, centerX + 2, centerY + 2);
    
    // Restore
    SelectObject(g_overlayDC, oldPen);
    
    // Draw status text
    HGDIOBJ oldFont = SelectObject(g_overlayDC, g_textFont);
    SetBkMode(g_overlayDC, TRANSPARENT);
    SetTextColor(g_overlayDC, RGB(255, 255, 255));
    
    std::wstring status = g_config.detectionEnabled ? L"ACTIVE" : L"INACTIVE";
    std::wstring movement = IsMovementKeyPressed() ? L"MOVING" : L"STATIONARY";
    
    std::wstring text = L"PixelTrigger Pro Enhanced\n" + status + 
                       L" | " + movement +
                       L"\nRadius: " + std::to_wstring(g_config.radius) +
                       L"\nTolerance: " + std::to_wstring(g_config.tolerance) +
                       L"\nClick Delay: " + std::to_wstring(g_config.clickDelay) + L"ms" +
                       L"\nCooldown: " + std::to_wstring(g_config.cooldownMs) + L"ms" +
                       L"\nCenter: (" + std::to_wstring(g_config.centerX) + 
                       L", " + std::to_wstring(g_config.centerY) + L")";
    
    RECT textRect = {10, 10, 400, 150};
    DrawText(g_overlayDC, text.c_str(), -1, &textRect, DT_LEFT);
    
    SelectObject(g_overlayDC, oldFont);
}

// ===================== WINDOW PROCEDURES =====================
LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            g_overlayDC = GetDC(hwnd);
            g_circlePen = CreatePen(PS_SOLID, g_config.overlayThickness, g_config.overlayColor);
            g_crosshairPen = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
            
            // Create font
            g_textFont = CreateFont(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            break;
            
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            DrawOverlay();
            EndPaint(hwnd, &ps);
            break;
        }
            
        case WM_DISPLAYCHANGE:
            InvalidateRect(hwnd, nullptr, TRUE);
            break;
            
        case WM_ERASEBKGND:
            return 1; // Prevent flicker
            
        case WM_DESTROY:
            if (g_textFont) DeleteObject(g_textFont);
            if (g_crosshairPen) DeleteObject(g_crosshairPen);
            if (g_circlePen) DeleteObject(g_circlePen);
            if (g_overlayDC) ReleaseDC(hwnd, g_overlayDC);
            PostQuitMessage(0);
            break;
            
        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

// ===================== COLOR PICKER DIALOG =====================
COLORREF ShowColorPicker(COLORREF initialColor) {
    CHOOSECOLOR cc = { sizeof(CHOOSECOLOR) };
    static COLORREF customColors[16] = {0};
    
    cc.hwndOwner = g_mainWindow;
    cc.lpCustColors = customColors;
    cc.rgbResult = initialColor;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    
    if (ChooseColor(&cc)) {
        return cc.rgbResult;
    }
    return initialColor;
}

// ===================== HOTKEY HANDLER =====================
void HandleHotkeys() {
    static bool f2Pressed = false;
    
    if (g_config.toggleHotkeyEnabled && (GetAsyncKeyState(g_config.toggleHotkey) & 0x8000)) {
        if (!f2Pressed) {
            g_config.detectionEnabled = !g_config.detectionEnabled;
            f2Pressed = true;
            
            // Update overlay
            if (g_overlayWindow) {
                InvalidateRect(g_overlayWindow, nullptr, TRUE);
            }
            
            // Update checkbox in main window
            HWND toggleCheck = GetDlgItem(g_mainWindow, 106);
            if (toggleCheck) {
                SendMessage(toggleCheck, BM_SETCHECK, g_config.detectionEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
            }
            
            // Show notification
            MessageBox(g_mainWindow, 
                       g_config.detectionEnabled ? 
                       L"CENTER DETECTION ENABLED\nWill click when color appears at screen center!\nWASD/Arrow keys disable clicking." :
                       L"CENTER DETECTION DISABLED\nNo clicking.",
                       L"PixelTrigger", MB_OK | MB_ICONINFORMATION);
        }
    } else {
        f2Pressed = false;
    }
}

// ===================== MAIN WINDOW CONTROLS =====================
void UpdateControls(HWND hwnd) {
    // Update radius slider
    HWND radiusSlider = GetDlgItem(hwnd, 101);
    SendMessage(radiusSlider, TBM_SETPOS, TRUE, g_config.radius);
    
    // Update tolerance slider
    HWND toleranceSlider = GetDlgItem(hwnd, 102);
    SendMessage(toleranceSlider, TBM_SETPOS, TRUE, g_config.tolerance);
    
    // Update cooldown slider
    HWND cooldownSlider = GetDlgItem(hwnd, 103);
    SendMessage(cooldownSlider, TBM_SETPOS, TRUE, g_config.cooldownMs);
    
    // NEW: Update click delay slider
    HWND clickDelaySlider = GetDlgItem(hwnd, 111);
    SendMessage(clickDelaySlider, TBM_SETPOS, TRUE, g_config.clickDelay);
    
    // Update checkboxes
    HWND clickCheck = GetDlgItem(hwnd, 104);
    SendMessage(clickCheck, BM_SETCHECK, g_config.clickWhenDetected ? BST_CHECKED : BST_UNCHECKED, 0);
    
    HWND overlayCheck = GetDlgItem(hwnd, 105);
    SendMessage(overlayCheck, BM_SETCHECK, g_config.showOverlay ? BST_CHECKED : BST_UNCHECKED, 0);
    
    HWND toggleCheck = GetDlgItem(hwnd, 106);
    SendMessage(toggleCheck, BM_SETCHECK, g_config.detectionEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
}

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            // Get screen center
            int screenWidth = GetSystemMetrics(SM_CXSCREEN);
            int screenHeight = GetSystemMetrics(SM_CYSCREEN);
            g_config.centerX = screenWidth / 2;
            g_config.centerY = screenHeight / 2;
            
            int yPos = 10;
            
            // Detection Radius
            CreateWindow(L"STATIC", L"Detection Radius:", WS_CHILD | WS_VISIBLE,
                        10, yPos, 120, 20, hwnd, NULL, NULL, NULL);
            CreateWindow(TRACKBAR_CLASS, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS,
                        10, yPos + 20, 200, 30, hwnd, (HMENU)101, NULL, NULL);
            yPos += 60;
            
            // Color Tolerance
            CreateWindow(L"STATIC", L"Color Tolerance:", WS_CHILD | WS_VISIBLE,
                        10, yPos, 120, 20, hwnd, NULL, NULL, NULL);
            CreateWindow(TRACKBAR_CLASS, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS,
                        10, yPos + 20, 200, 30, hwnd, (HMENU)102, NULL, NULL);
            yPos += 60;
            
            // Click Cooldown
            CreateWindow(L"STATIC", L"Click Cooldown (ms):", WS_CHILD | WS_VISIBLE,
                        10, yPos, 120, 20, hwnd, NULL, NULL, NULL);
            CreateWindow(TRACKBAR_CLASS, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS,
                        10, yPos + 20, 200, 30, hwnd, (HMENU)103, NULL, NULL);
            yPos += 60;
            
            // NEW: Click Delay After Detection
            CreateWindow(L"STATIC", L"Click Delay (ms):", WS_CHILD | WS_VISIBLE,
                        10, yPos, 120, 20, hwnd, NULL, NULL, NULL);
            CreateWindow(TRACKBAR_CLASS, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS,
                        10, yPos + 20, 200, 30, hwnd, (HMENU)111, NULL, NULL);
            yPos += 60;
            
            // Checkboxes
            CreateWindow(L"BUTTON", L"Click when detected", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                        10, yPos, 150, 20, hwnd, (HMENU)104, NULL, NULL);
            yPos += 30;
            
            CreateWindow(L"BUTTON", L"Show overlay", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                        10, yPos, 150, 20, hwnd, (HMENU)105, NULL, NULL);
            yPos += 30;
            
            CreateWindow(L"BUTTON", L"Enable detection", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                        10, yPos, 150, 20, hwnd, (HMENU)106, NULL, NULL);
            yPos += 40;
            
            // Buttons row 1
            CreateWindow(L"BUTTON", L"Pick Target Color", WS_CHILD | WS_VISIBLE,
                        10, yPos, 120, 30, hwnd, (HMENU)107, NULL, NULL);
            
            CreateWindow(L"BUTTON", L"Toggle (F2)", WS_CHILD | WS_VISIBLE,
                        140, yPos, 100, 30, hwnd, (HMENU)108, NULL, NULL);
            yPos += 40;
            
            // Buttons row 2
            CreateWindow(L"BUTTON", L"Test Click NOW", WS_CHILD | WS_VISIBLE,
                        10, yPos, 120, 30, hwnd, (HMENU)109, NULL, NULL);
            
            CreateWindow(L"BUTTON", L"Test WASD Lock", WS_CHILD | WS_VISIBLE,
                        140, yPos, 100, 30, hwnd, (HMENU)110, NULL, NULL);
            yPos += 40;
            
            // Center coordinates display
            std::wstring centerText = L"Screen Center: (" + 
                                     std::to_wstring(g_config.centerX) + L", " + 
                                     std::to_wstring(g_config.centerY) + L")";
            
            CreateWindow(L"STATIC", centerText.c_str(), WS_CHILD | WS_VISIBLE,
                        10, yPos, 250, 20, hwnd, NULL, NULL, NULL);
            yPos += 25;
            
            // Movement lock info
            CreateWindow(L"STATIC", L"WASD/Arrow keys disable clicking", 
                        WS_CHILD | WS_VISIBLE,
                        10, yPos, 250, 20, hwnd, NULL, NULL, NULL);
            yPos += 25;
            
            // Hotkey info
            CreateWindow(L"STATIC", L"F2: Toggle | Click Delay: 0-500ms", 
                        WS_CHILD | WS_VISIBLE,
                        10, yPos, 250, 20, hwnd, NULL, NULL, NULL);
            
            // Configure sliders
            HWND radiusSlider = GetDlgItem(hwnd, 101);
            SendMessage(radiusSlider, TBM_SETRANGE, TRUE, MAKELONG(1, 30));
            SendMessage(radiusSlider, TBM_SETTICFREQ, 5, 0);
            
            HWND toleranceSlider = GetDlgItem(hwnd, 102);
            SendMessage(toleranceSlider, TBM_SETRANGE, TRUE, MAKELONG(0, 100));
            SendMessage(toleranceSlider, TBM_SETTICFREQ, 10, 0);
            
            HWND cooldownSlider = GetDlgItem(hwnd, 103);
            SendMessage(cooldownSlider, TBM_SETRANGE, TRUE, MAKELONG(0, 500));
            SendMessage(cooldownSlider, TBM_SETTICFREQ, 50, 0);
            
            // NEW: Click delay slider
            HWND clickDelaySlider = GetDlgItem(hwnd, 111);
            SendMessage(clickDelaySlider, TBM_SETRANGE, TRUE, MAKELONG(0, 500));
            SendMessage(clickDelaySlider, TBM_SETTICFREQ, 50, 0);
            
            UpdateControls(hwnd);
            break;
        }
        
        case WM_HSCROLL: {
            HWND slider = (HWND)lParam;
            int value = (int)SendMessage(slider, TBM_GETPOS, 0, 0);
            
            if (slider == GetDlgItem(hwnd, 101)) {
                g_config.radius = value;
            } else if (slider == GetDlgItem(hwnd, 102)) {
                g_config.tolerance = value;
            } else if (slider == GetDlgItem(hwnd, 103)) {
                g_config.cooldownMs = value;
            } else if (slider == GetDlgItem(hwnd, 111)) { // NEW: Click delay slider
                g_config.clickDelay = value;
            }
            
            // Update overlay
            if (g_overlayWindow) {
                InvalidateRect(g_overlayWindow, nullptr, TRUE);
            }
            break;
        }
        
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            
            if (id == 104) { // Click checkbox
                g_config.clickWhenDetected = !g_config.clickWhenDetected;
            } else if (id == 105) { // Overlay checkbox
                g_config.showOverlay = !g_config.showOverlay;
                ShowWindow(g_overlayWindow, g_config.showOverlay ? SW_SHOW : SW_HIDE);
            } else if (id == 106) { // Toggle checkbox
                g_config.detectionEnabled = !g_config.detectionEnabled;
                UpdateControls(hwnd);
            } else if (id == 107) { // Pick color
                g_config.targetColor = ShowColorPicker(g_config.targetColor);
                // Update circle color
                if (g_circlePen) {
                    DeleteObject(g_circlePen);
                    g_circlePen = CreatePen(PS_SOLID, g_config.overlayThickness, g_config.targetColor);
                }
            } else if (id == 108) { // Manual toggle
                g_config.detectionEnabled = !g_config.detectionEnabled;
                UpdateControls(hwnd);
                MessageBox(hwnd, 
                          g_config.detectionEnabled ? 
                          L"CENTER DETECTION ENABLED\nWASD/Arrow keys disable clicking." :
                          L"CENTER DETECTION DISABLED",
                          L"PixelTrigger", MB_OK | MB_ICONINFORMATION);
            } else if (id == 109) { // Test click
                PerformGuaranteedClick();
                MessageBox(hwnd, L"TEST CLICK PERFORMED!\nCheck if it worked.", L"PixelTrigger", MB_OK | MB_ICONINFORMATION);
            } else if (id == 110) { // Test WASD lock
                MessageBox(hwnd, 
                          L"WASD LOCK TEST:\n"
                          L"1. Hold W, A, S, D, or Arrow keys\n"
                          L"2. The bot will NOT click while keys are held\n"
                          L"3. Release keys to resume normal operation",
                          L"PixelTrigger", MB_OK | MB_ICONINFORMATION);
            }
            break;
        }
        
        case WM_CLOSE:
            DestroyWindow(hwnd);
            break;
            
        case WM_DESTROY:
            g_running = false;
            PostQuitMessage(0);
            break;
            
        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

// ===================== OVERLAY WINDOW CREATION =====================
bool CreateOverlayWindow() {
    WNDCLASS wc = {};
    wc.lpfnWndProc = OverlayWndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"PixelTriggerOverlay";
    
    RegisterClass(&wc);
    
    // Get screen size
    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    
    // Create transparent layered window
    g_overlayWindow = CreateWindowEx(
        WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_TOOLWINDOW,
        L"PixelTriggerOverlay",
        L"PixelTrigger Overlay",
        WS_POPUP,
        0, 0, screenWidth, screenHeight,
        nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
    
    if (!g_overlayWindow) return false;
    
    // Set transparency
    SetLayeredWindowAttributes(g_overlayWindow, RGB(0, 0, 0), 0, LWA_COLORKEY);
    
    ShowWindow(g_overlayWindow, SW_SHOW);
    UpdateWindow(g_overlayWindow);
    
    return true;
}

// ===================== MAIN ENTRY POINT =====================
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    // Initialize common controls
    INITCOMMONCONTROLSEX icc = { sizeof(INITCOMMONCONTROLSEX), ICC_WIN95_CLASSES };
    InitCommonControlsEx(&icc);
    
    // Register main window class
    WNDCLASS wc = {};
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"PixelTriggerMain";
    
    RegisterClass(&wc);
    
    // Create main window
    g_mainWindow = CreateWindow(
        L"PixelTriggerMain",
        L"PixelTrigger Pro Enhanced",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 320, 550,
        nullptr, nullptr, hInstance, nullptr);
    
    if (!g_mainWindow) return 1;
    
    ShowWindow(g_mainWindow, nCmdShow);
    UpdateWindow(g_mainWindow);
    
    // Create overlay window
    if (!CreateOverlayWindow()) {
        MessageBox(nullptr, L"Failed to create overlay window!", L"Error", MB_ICONERROR);
        return 1;
    }
    
    // Start capture thread
    g_captureThreadRunning = true;
    g_captureThread = std::thread(CaptureThread);
    
    // Main message loop
    MSG msg = {};
    while (g_running) {
        // Check for hotkeys
        HandleHotkeys();
        
        // Process messages
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            
            if (msg.message == WM_QUIT) {
                g_running = false;
            }
        }
        
        // Update overlay
        if (g_overlayWindow && g_config.showOverlay) {
            InvalidateRect(g_overlayWindow, nullptr, TRUE);
            UpdateWindow(g_overlayWindow);
        }
        
        // Small delay to prevent high CPU usage
        Sleep(10);
    }
    
    // Cleanup
    g_captureThreadRunning = false;
    if (g_captureThread.joinable()) {
        g_captureThread.join();
    }
    
    if (g_overlayWindow) DestroyWindow(g_overlayWindow);
    
    return 0;
}