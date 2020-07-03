// Copyright (c) 2014-2017 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "activeznode.h"
#include "consensus/consensus.h"
#include "znode.h"
#include "protocol.h"
#include "netbase.h"

// TODO: upgrade to new dash, remove this hack
#define vNodes (g_connman->vNodes)
#define cs_vNodes (g_connman->cs_vNodes)

extern CWallet *pwalletMain;

// Keep track of the active Znode
CActiveZnode activeZnode;

void CActiveZnode::ManageState() {
    LogPrint("znode", "CActiveZnode::ManageState -- Start\n");
    if (!fMasternodeMode) {
        LogPrint("znode", "CActiveZnode::ManageState -- Not a znode, returning\n");
        return;
    }

    if (nState == ACTIVE_ZNODE_SYNC_IN_PROCESS) {
        nState = ACTIVE_ZNODE_INITIAL;
    }

    LogPrint("znode", "CActiveZnode::ManageState -- status = %s, type = %s, pinger enabled = %d\n",
             GetStatus(), GetTypeString(), fPingerEnabled);

    if (eType == ZNODE_UNKNOWN) {
        ManageStateInitial();
    }

    if (eType == ZNODE_REMOTE) {
        ManageStateRemote();
    } else if (eType == ZNODE_LOCAL) {
        // Try Remote Start first so the started local znode can be restarted without recreate znode broadcast.
        ManageStateRemote();
        if (nState != ACTIVE_ZNODE_STARTED)
            ManageStateLocal();
    }

    SendZnodePing();
}

std::string CActiveZnode::GetStateString() const {
    switch (nState) {
        case ACTIVE_ZNODE_INITIAL:
            return "INITIAL";
        case ACTIVE_ZNODE_SYNC_IN_PROCESS:
            return "SYNC_IN_PROCESS";
        case ACTIVE_ZNODE_INPUT_TOO_NEW:
            return "INPUT_TOO_NEW";
        case ACTIVE_ZNODE_NOT_CAPABLE:
            return "NOT_CAPABLE";
        case ACTIVE_ZNODE_STARTED:
            return "STARTED";
        default:
            return "UNKNOWN";
    }
}

std::string CActiveZnode::GetStatus() const {
    switch (nState) {
        case ACTIVE_ZNODE_INITIAL:
            return "Node just started, not yet activated";
        case ACTIVE_ZNODE_SYNC_IN_PROCESS:
            return "Sync in progress. Must wait until sync is complete to start Znode";
        case ACTIVE_ZNODE_INPUT_TOO_NEW:
            return strprintf("Znode input must have at least %d confirmations",
                             Params().GetConsensus().nZnodeMinimumConfirmations);
        case ACTIVE_ZNODE_NOT_CAPABLE:
            return "Not capable znode: " + strNotCapableReason;
        case ACTIVE_ZNODE_STARTED:
            return "Znode successfully started";
        default:
            return "Unknown";
    }
}

std::string CActiveZnode::GetTypeString() const {
    std::string strType;
    switch (eType) {
        case ZNODE_UNKNOWN:
            strType = "UNKNOWN";
            break;
        case ZNODE_REMOTE:
            strType = "REMOTE";
            break;
        case ZNODE_LOCAL:
            strType = "LOCAL";
            break;
        default:
            strType = "UNKNOWN";
            break;
    }
    return strType;
}

bool CActiveZnode::SendZnodePing() {
    return true;
}

void CActiveZnode::ManageStateInitial() {
    LogPrint("znode", "CActiveZnode::ManageStateInitial -- status = %s, type = %s, pinger enabled = %d\n",
             GetStatus(), GetTypeString(), fPingerEnabled);

    // Check that our local network configuration is correct
    if (!fListen) {
        // listen option is probably overwritten by smth else, no good
        nState = ACTIVE_ZNODE_NOT_CAPABLE;
        strNotCapableReason = "Znode must accept connections from outside. Make sure listen configuration option is not overwritten by some another parameter.";
        LogPrintf("CActiveZnode::ManageStateInitial -- %s: %s\n", GetStateString(), strNotCapableReason);
        return;
    }

    bool fFoundLocal = false;
    {
        LOCK(cs_vNodes);

        // First try to find whatever local address is specified by externalip option
        fFoundLocal = GetLocal(service) && CZnode::IsValidNetAddr(service);
        if (!fFoundLocal) {
            // nothing and no live connections, can't do anything for now
            if (vNodes.empty()) {
                nState = ACTIVE_ZNODE_NOT_CAPABLE;
                strNotCapableReason = "Can't detect valid external address. Will retry when there are some connections available.";
                LogPrintf("CActiveZnode::ManageStateInitial -- %s: %s\n", GetStateString(), strNotCapableReason);
                return;
            }
            // We have some peers, let's try to find our local address from one of them
            BOOST_FOREACH(CNode * pnode, vNodes)
            {
                if (pnode->fSuccessfullyConnected && pnode->addr.IsIPv4()) {
                    fFoundLocal = GetLocal(service, &pnode->addr) && CZnode::IsValidNetAddr(service);
                    if (fFoundLocal) break;
                }
            }
        }
    }
        
    if(Params().NetworkIDString() == CBaseChainParams::REGTEST) {
        std::string const & serv = GetArg("-externalip", "");
        if(!serv.empty()) {
            if (Lookup(serv.c_str(), service, 0, false))
                fFoundLocal = true;
        }

    }
    if(!fFoundLocal)
    {
        LOCK(cs_vNodes);

        // First try to find whatever local address is specified by externalip option
        fFoundLocal = GetLocal(service) && CZnode::IsValidNetAddr(service);
        if (!fFoundLocal) {
            // nothing and no live connections, can't do anything for now
            if (vNodes.empty()) {
                nState = ACTIVE_ZNODE_NOT_CAPABLE;
                strNotCapableReason = "Can't detect valid external address. Will retry when there are some connections available.";
                LogPrintf("CActiveZnode::ManageStateInitial -- %s: %s\n", GetStateString(), strNotCapableReason);
                return;
            }
            // We have some peers, let's try to find our local address from one of them
            BOOST_FOREACH(CNode * pnode, vNodes)
            {
                if (pnode->fSuccessfullyConnected && pnode->addr.IsIPv4()) {
                    fFoundLocal = GetLocal(service, &pnode->addr) && CZnode::IsValidNetAddr(service);
                    if (fFoundLocal) break;
                }
            }
        }
    }

    if (!fFoundLocal) {
        nState = ACTIVE_ZNODE_NOT_CAPABLE;
        strNotCapableReason = "Can't detect valid external address. Please consider using the externalip configuration option if problem persists. Make sure to use IPv4 address only.";
        LogPrintf("CActiveZnode::ManageStateInitial -- %s: %s\n", GetStateString(), strNotCapableReason);
        return;
    }

    int mainnetDefaultPort = Params(CBaseChainParams::MAIN).GetDefaultPort();
    if (Params().NetworkIDString() == CBaseChainParams::MAIN) {
        if (service.GetPort() != mainnetDefaultPort) {
            nState = ACTIVE_ZNODE_NOT_CAPABLE;
            strNotCapableReason = strprintf("Invalid port: %u - only %d is supported on mainnet.", service.GetPort(),
                                            mainnetDefaultPort);
            LogPrintf("CActiveZnode::ManageStateInitial -- %s: %s\n", GetStateString(), strNotCapableReason);
            return;
        }
    } else if (service.GetPort() == mainnetDefaultPort) {
        nState = ACTIVE_ZNODE_NOT_CAPABLE;
        strNotCapableReason = strprintf("Invalid port: %u - %d is only supported on mainnet.", service.GetPort(),
                                        mainnetDefaultPort);
        LogPrintf("CActiveZnode::ManageStateInitial -- %s: %s\n", GetStateString(), strNotCapableReason);
        return;
    }

    LogPrintf("CActiveZnode::ManageStateInitial -- Checking inbound connection to '%s'\n", service.ToString());
    //TODO
    if (!g_connman->OpenMasternodeConnection(CAddress(service, NODE_NETWORK))) {
        nState = ACTIVE_ZNODE_NOT_CAPABLE;
        strNotCapableReason = "Could not connect to " + service.ToString();
        LogPrintf("CActiveZnode::ManageStateInitial -- %s: %s\n", GetStateString(), strNotCapableReason);
        return;
    }

    // Default to REMOTE
    eType = ZNODE_REMOTE;

    // Check if wallet funds are available
    if (!pwalletMain) {
        LogPrintf("CActiveZnode::ManageStateInitial -- %s: Wallet not available\n", GetStateString());
        return;
    }

    if (pwalletMain->IsLocked()) {
        LogPrintf("CActiveZnode::ManageStateInitial -- %s: Wallet is locked\n", GetStateString());
        return;
    }

    if (pwalletMain->GetBalance() < ZNODE_COIN_REQUIRED * COIN) {
        LogPrintf("CActiveZnode::ManageStateInitial -- %s: Wallet balance is < 1000 XZC\n", GetStateString());
        return;
    }

    // Choose coins to use
    CPubKey pubKeyCollateral;
    CKey keyCollateral;

    // If collateral is found switch to LOCAL mode
    if (pwalletMain->GetZnodeVinAndKeys(vin, pubKeyCollateral, keyCollateral)) {
        eType = ZNODE_LOCAL;
    }

    LogPrint("znode", "CActiveZnode::ManageStateInitial -- End status = %s, type = %s, pinger enabled = %d\n",
             GetStatus(), GetTypeString(), fPingerEnabled);
}

void CActiveZnode::ManageStateRemote() {
}

void CActiveZnode::ManageStateLocal() {
    LogPrint("znode", "CActiveZnode::ManageStateLocal -- status = %s, type = %s, pinger enabled = %d\n",
             GetStatus(), GetTypeString(), fPingerEnabled);
    if (nState == ACTIVE_ZNODE_STARTED) {
        return;
    }

    // Choose coins to use
    CPubKey pubKeyCollateral;
    CKey keyCollateral;

    if (pwalletMain->GetZnodeVinAndKeys(vin, pubKeyCollateral, keyCollateral)) {
        int nInputAge = GetInputAge(vin);
        if (nInputAge < Params().GetConsensus().nZnodeMinimumConfirmations) {
            nState = ACTIVE_ZNODE_INPUT_TOO_NEW;
            strNotCapableReason = strprintf(_("%s - %d confirmations"), GetStatus(), nInputAge);
            LogPrintf("CActiveZnode::ManageStateLocal -- %s: %s\n", GetStateString(), strNotCapableReason);
            return;
        }

        {
            LOCK(pwalletMain->cs_wallet);
            pwalletMain->LockCoin(vin.prevout);
        }

        CZnodeBroadcast mnb;
        std::string strError;
        if (!CZnodeBroadcast::Create(vin, service, keyCollateral, pubKeyCollateral, keyZnode,
                                     pubKeyZnode, strError, mnb)) {
            nState = ACTIVE_ZNODE_NOT_CAPABLE;
            strNotCapableReason = "Error creating mastenode broadcast: " + strError;
            LogPrintf("CActiveZnode::ManageStateLocal -- %s: %s\n", GetStateString(), strNotCapableReason);
            return;
        }

        fPingerEnabled = true;
        nState = ACTIVE_ZNODE_STARTED;

        //update to znode list
        LogPrintf("CActiveZnode::ManageStateLocal -- Update Znode List\n");

        //send to all peers
        LogPrintf("CActiveZnode::ManageStateLocal -- Relay broadcast, vin=%s\n", vin.ToString());
        mnb.RelayZNode();
    }
}
