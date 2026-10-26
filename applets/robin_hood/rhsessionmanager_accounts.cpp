#include "rhsessionmanager.h"

#include <QLocale>
#include <QRegularExpression>
#include <QStringList>

#include <limits>
#include <utility>

namespace {
QString money(const QJsonValue &value)
{
    if (!value.isString() && !value.isDouble())
        return {};
    static const QRegularExpression decimal(QStringLiteral(R"(^\s*(-?)(\d+)(?:\.(\d+))?\s*$)"));
    const auto match = decimal.match(value.toVariant().toString());
    if (!match.hasMatch())
        return {};
    bool valid = false;
    qint64 whole = match.captured(2).toLongLong(&valid);
    if (!valid || whole == std::numeric_limits<qint64>::max())
        return {};
    const QString digits = match.captured(3);
    int cents = digits.left(2).leftJustified(2, QLatin1Char('0')).toInt();
    if (digits.size() > 2 && digits.at(2) >= QLatin1Char('5'))
        ++cents;
    if (cents == 100) {
        ++whole;
        cents = 0;
    }
    const QString sign = match.captured(1) == QStringLiteral("-") && (whole || cents)
        ? QStringLiteral("-") : QString();
    return sign + QStringLiteral("$")
        + QLocale(QLocale::English, QLocale::UnitedStates).toString(whole)
        + QStringLiteral(".") + QStringLiteral("%1").arg(cents, 2, 10, QLatin1Char('0'));
}

QString balance(const QJsonObject &item)
{
    for (const char *key : {"total_equity", "equity", "portfolio_equity"}) {
        const QString amount = money(item.value(QLatin1String(key)));
        if (!amount.isEmpty())
            return amount;
    }
    // Cash is not total equity for a margin account holding securities.
    if (item.value(QStringLiteral("type")).toString().compare(QStringLiteral("margin"), Qt::CaseInsensitive) != 0) {
        const QJsonValue cash = item.value(QStringLiteral("cash_balances"));
        if (cash.isObject()) {
            for (const char *key : {"cash", "total_cash", "cash_balance"}) {
                const QString amount = money(cash.toObject().value(QLatin1String(key)));
                if (!amount.isEmpty())
                    return amount;
            }
        } else if (const QString amount = money(cash); !amount.isEmpty()) {
            return amount;
        }
    }
    return QStringLiteral("Unavailable");
}

QString displayName(const QJsonObject &item)
{
    QStringList labels;
    for (const char *key : {"type", "rh_account_type", "account_type", "description", "nickname", "retirement_type"})
        labels.append(item.value(QLatin1String(key)).toString());
    const QJsonValue retirement = item.value(QStringLiteral("retirement"));
    if (retirement.isObject()) {
        labels.append(QStringLiteral("ira"));
        const QJsonObject details = retirement.toObject();
        for (const char *key : {"type", "rh_account_type", "account_type", "subtype", "description"})
            labels.append(details.value(QLatin1String(key)).toString());
    } else if (retirement.isString()) {
        labels.append(retirement.toString());
    }
    const QString normalized = labels.join(QLatin1Char('_')).toLower().replace(
        QRegularExpression(QStringLiteral("[^a-z0-9]+")), QStringLiteral("_"));
    if (normalized.contains(QStringLiteral("roth_ira")) || normalized.contains(QStringLiteral("ira_roth")))
        return QStringLiteral("Roth IRA");
    if (normalized.contains(QStringLiteral("traditional_ira")) || normalized.contains(QStringLiteral("ira_traditional")))
        return QStringLiteral("Traditional IRA");
    if (retirement.isObject() || retirement.isString() || item.value(QStringLiteral("is_retirement")).toBool()
        || normalized.contains(QStringLiteral("_ira")) || normalized.contains(QStringLiteral("_retirement")))
        return QStringLiteral("Retirement Account");
    if (item.value(QStringLiteral("type")).toString().compare(QStringLiteral("margin"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("Individual Investing");
    for (const char *key : {"nickname", "description"}) {
        const QString label = item.value(QLatin1String(key)).toString().trimmed();
        if (!label.isEmpty() && label.compare(QStringLiteral("cash"), Qt::CaseInsensitive) != 0)
            return label;
    }
    return item.value(QStringLiteral("type")).toString().compare(QStringLiteral("cash"), Qt::CaseInsensitive) == 0
        ? QStringLiteral("Cash Account") : QStringLiteral("Brokerage Account");
}
}

int RHAccountModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_items.size();
}

QVariant RHAccountModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_items.size())
        return {};
    const Account &account = m_items.at(index.row());
    switch (role) {
    case DisplayNameRole: return account.displayName;
    case AccountNumberRole: return account.accountNumber;
    case CurrentBalanceRole: return account.currentBalance;
    default: return {};
    }
}

QHash<int, QByteArray> RHAccountModel::roleNames() const
{
    return {{DisplayNameRole, "displayName"}, {AccountNumberRole, "accountNumber"},
            {CurrentBalanceRole, "currentBalance"}};
}

void RHAccountModel::setAccounts(const QJsonArray &results)
{
    QList<Account> accounts;
    for (const QJsonValue &value : results) {
        if (!value.isObject())
            continue;
        const QJsonObject item = value.toObject();
        QString accountNumber = item.value(QStringLiteral("account_number")).toString();
        if (accountNumber.isEmpty())
            accountNumber = item.value(QStringLiteral("rhs_account_number")).toString();
        accounts.append({displayName(item), accountNumber, balance(item)});
    }
    beginResetModel();
    m_items = std::move(accounts);
    endResetModel();
}

void RHAccountModel::clear()
{
    if (m_items.isEmpty())
        return;
    beginResetModel();
    m_items.clear();
    endResetModel();
}
