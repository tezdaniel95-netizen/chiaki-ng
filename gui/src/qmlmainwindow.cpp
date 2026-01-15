#include "qmlmainwindow.h"
#include "qmlbackend.h"
#include "qmlsvgprovider.h"
#include "chiaki/log.h"
#include "streamsession.h"

#include <qpa/qplatformnativeinterface.h>

#define PL_LIBAV_IMPLEMENTATION 0
#include <libplacebo/utils/libav.h>

#include <QDebug>
#include <QThread>
#include <QShortcut>
#include <QStandardPaths>
#include <QGuiApplication>
#include <QVulkanInstance>
#include <QQuickItem>
#include <QQmlEngine>
#include <QQmlComponent>
#include <QQuickRenderTarget>
#include <QQuickRenderControl>
#include <QQuickGraphicsDevice>

// --- NOVO: NECESSÁRIO PARA A MIRA FUNCIONAR ---
#include <QMouseEvent>
#include <QCursor>
// ----------------------------------------------

#if defined(Q_OS_MACOS)
#include <objc/message.h>
#endif
// =================================================================
// CÓDIGO DE INJEÇÃO DE MOUSE (MIRA NATIVA)
// =================================================================

bool QmlMainWindow::event(QEvent *ev)
{
    // A mágica só acontece se:
    // 1. O evento for movimento de mouse
    // 2. O jogo estiver rodando (session e has_video)
    // 3. O input estiver capturado (Você apertou Ctrl+F11 ou tocou na tela para prender o mouse)
    if (ev->type() == QEvent::MouseMove && session && has_video && grab_input) {
        QMouseEvent *me = static_cast<QMouseEvent*>(ev);
        
        // --- CONFIGURAÇÕES (Edite aqui para ajustar) ---
        int sensitivity = 450; // Velocidade da mira (Tente 300 a 800)
        int deadzone = 2200;   // Remove o peso inicial do analógico
        
        // Pega o centro da janela
        int centerX = width() / 2;
        int centerY = height() / 2;
        
        // Calcula quanto o mouse andou
        int deltaX = me->x() - centerX;
        int deltaY = me->y() - centerY;

        // Se o mouse não mexeu, retorna (evita tremedeira)
        if (deltaX == 0 && deltaY == 0) return true;

        // Aplica a sensibilidade
        int finalX = deltaX * sensitivity;
        int finalY = deltaY * sensitivity;

        // --- ANTI-DEADZONE (O Segredo da fluidez) ---
        // Empurra a mira pra fora da zona morta instantaneamente
        if (finalX > 0) finalX += deadzone;
        if (finalX < 0) finalX -= deadzone;
        if (finalY > 0) finalY += deadzone;
        if (finalY < 0) finalY -= deadzone;

        // Trava no limite máximo do controle do PS5
        if (finalX > 32767) finalX = 32767;
        if (finalX < -32767) finalX = -32767;
        if (finalY > 32767) finalY = 32767;
        if (finalY < -32767) finalY = -32767;

        // Envia o comando direto para o PS5 (Analógico Direito)
        session->controllerState().rightStickX = finalX;
        session->controllerState().rightStickY = finalY;

        // Reseta o mouse para o centro da tela (Loop Infinito)
        // Isso permite você girar 360 graus sem o mouse bater na borda do monitor
        QCursor::setPos(mapToGlobal(QPoint(centerX, centerY)));
        
        return true; // Avisa que nós controlamos esse evento
    }

    // Se não for mouse, deixa o Chiaki funcionar normalmente
    return QWindow::event(ev);
}
