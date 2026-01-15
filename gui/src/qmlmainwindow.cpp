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

// --- ADICIONADO PARA CORRIGIR O ERRO ---
#include <QMouseEvent>
#include <QCursor>
// ---------------------------------------

#if defined(Q_OS_MACOS)
#include <objc/message.h>
#endif

#if defined(Q_OS_MACOS)
#include <objc/message.h>
#endif
// =================================================================
// CÓDIGO DE MIRA (MOUSE AIM) - VERSÃO CORRIGIDA FINAL
// =================================================================
bool QmlMainWindow::event(QEvent *ev)
{
    // Verifica mouse, vídeo e captura
    if (ev->type() == QEvent::MouseMove && session && has_video && grab_input) {
        QMouseEvent *me = static_cast<QMouseEvent*>(ev);
        
        // --- CONFIGURAÇÕES ---
        int sensitivity = 450; // Ajuste a velocidade aqui
        int deadzone = 2200;   
        
        // Pega o centro da janela
        int centerX = width() / 2;
        int centerY = height() / 2;
        
        // CORREÇÃO FINAL: Usa position().x()
        int deltaX = me->position().x() - centerX;
        int deltaY = me->position().y() - centerY;

        if (deltaX == 0 && deltaY == 0) return true;

        // Sensibilidade
        int finalX = deltaX * sensitivity;
        int finalY = deltaY * sensitivity;

        // Anti-Deadzone
        if (finalX > 0) finalX += deadzone;
        if (finalX < 0) finalX -= deadzone;
        if (finalY > 0) finalY += deadzone;
        if (finalY < -2200) finalY -= deadzone; 

        // Limites do Controle
        if (finalX > 32767) finalX = 32767;
        if (finalX < -32767) finalX = -32767;
        if (finalY > 32767) finalY = 32767;
        if (finalY < -32767) finalY = -32767;

        // --- A MÁGICA ---
        // Agora o comando está 100% correto
        session->GetAimState().rightStickX = finalX;
        session->GetAimState().rightStickY = finalY;
        
        // Força envio
        session->ForceSendInput();

        // Reseta o mouse
        QCursor::setPos(mapToGlobal(QPoint(centerX, centerY)));
        
        return true; 
    }

    return QWindow::event(ev);
}
