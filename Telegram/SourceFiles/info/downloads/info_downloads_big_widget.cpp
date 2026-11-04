/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "info/downloads/info_downloads_big_widget.h"

#include "info/info_controller.h"
#include "info/info_memento.h"
#include "data/data_user.h"
#include "lang/lang_keys.h"

namespace Info::DownloadsBig {

Memento::Memento(not_null<UserData*> self)
: ContentMemento(DownloadsBig::Tag()) {
}

Memento::~Memento() = default;

object_ptr<ContentWidget> Memento::createWidget(
		QWidget *parent,
		not_null<Controller*> controller,
		const QRect &geometry) {
	auto result = object_ptr<Widget>(parent, controller);
	result->setInternalState(geometry, this);
	return result;
}

Section Memento::section() const {
	return Section(Section::Type::DownloadsBig);
}

Widget::Widget(QWidget *parent, not_null<Controller*> controller)
: ContentWidget(parent, controller) {
}

bool Widget::showInternal(not_null<ContentMemento*> memento) {
	return true;
}

void Widget::setInternalState(
		const QRect &geometry,
		not_null<Memento*> memento) {
	resize(geometry.size());
}

rpl::producer<QString> Widget::title() {
	return tr::lng_downloads_section();
}

void Widget::saveState(not_null<Memento*> memento) {
}

void Widget::restoreState(not_null<Memento*> memento) {
}

std::shared_ptr<ContentMemento> Widget::doCreateMemento() {
	return std::make_shared<Memento>(
		controller()->session().user());
}

std::shared_ptr<Info::Memento> Make(not_null<UserData*> self) {
	return std::make_shared<Info::Memento>(
		std::vector<std::shared_ptr<ContentMemento>>(
			1,
			std::make_shared<Memento>(self)));
}

} // namespace Info::DownloadsBig
