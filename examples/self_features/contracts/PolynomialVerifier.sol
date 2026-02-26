// SPDX-License-Identifier: GPL-3.0
/*
    Copyright 2021 0KIMS association.

    This file is generated with [snarkJS](https://github.com/iden3/snarkjs).

    snarkJS is a free software: you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    snarkJS is distributed in the hope that it will be useful, but WITHOUT
    ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
    or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public
    License for more details.

    You should have received a copy of the GNU General Public License
    along with snarkJS. If not, see <https://www.gnu.org/licenses/>.
*/

pragma solidity >=0.7.0 <0.9.0;

contract PolynomialVerifier {
    // Scalar field size
    uint256 constant r    = 21888242871839275222246405745257275088548364400416034343698204186575808495617;
    // Base field size
    uint256 constant q   = 21888242871839275222246405745257275088696311157297823662689037894645226208583;

    // Verification Key data
    uint256 constant alphax  = 16452956278457028550524339868168502099673284249012355587838721146439548376906;
    uint256 constant alphay  = 1375574264994866309441128838579299768916464022373152821540478552336739698530;
    uint256 constant betax1  = 16904359728050582038646296275846568900318166129400708702633663131676522886354;
    uint256 constant betax2  = 11945235682535976921593361041895620031376917018792322786500169233923976347590;
    uint256 constant betay1  = 11572570617442237469942624689191799986236760659662307548181519826854111782885;
    uint256 constant betay2  = 10687210310029001332581963387554913617759220519841782180334298174830973822391;
    uint256 constant gammax1 = 11559732032986387107991004021392285783925812861821192530917403151452391805634;
    uint256 constant gammax2 = 10857046999023057135944570762232829481370756359578518086990519993285655852781;
    uint256 constant gammay1 = 4082367875863433681332203403145435568316851327593401208105741076214120093531;
    uint256 constant gammay2 = 8495653923123431417604973247489272438418190587263600148770280649306958101930;
    uint256 constant deltax1 = 18143026977456583976927909131107577409093961393560385201441913052341524948321;
    uint256 constant deltax2 = 18197037892391606212536814564561507592120309406969828754599798255515818830235;
    uint256 constant deltay1 = 8698989573497940685817346865808945770891905380268064537425808822034445514505;
    uint256 constant deltay2 = 12544737385209699439590369473470424386166369546927682781398611296217056795955;

    
    uint256 constant IC0x = 6764766207067798101620715573331950644000783232401981743502131867424390794748;
    uint256 constant IC0y = 4981659011429336731289465212999381830443397094519959208883455249395664861782;
    
    uint256 constant IC1x = 5677288910659011687052444784796635502852047379589822725104248030927843808483;
    uint256 constant IC1y = 4437059016868221969499464616234793328194641788486092691520568495872191773477;
    
    uint256 constant IC2x = 10098389101112349489930392842738642133956019398388839286607279650708704705059;
    uint256 constant IC2y = 13523484197364963234279747065027479048093240002488892276221491111309280960155;
    
    uint256 constant IC3x = 5310504417200260212760935401642278763309919299039058917991266152349209270583;
    uint256 constant IC3y = 11945575514698661728851734397500802250482234555662813563679183646056324730025;
    
    uint256 constant IC4x = 9421471160190090383064487623038993197410047629024440830810994905655178077574;
    uint256 constant IC4y = 3966472235973289612167579645503227731152272475605954956927386833794731342684;
    
    uint256 constant IC5x = 6053487447816622395062433281977770376381449592565065902122130617460693972292;
    uint256 constant IC5y = 6308864047447736838063319372801424143488115687201920087264101166167405525540;
    
    uint256 constant IC6x = 6119860687789316516279768234877132508976662445204061317658876300328937919573;
    uint256 constant IC6y = 4131906406548819012458918064670295727141055570520537866796275312728430648792;
    
    uint256 constant IC7x = 3433176621561978582148901127713215354701111235991974692118656806733140418721;
    uint256 constant IC7y = 14729568379748581237910248396890825441652700245136778117747240753527929480786;
    
    uint256 constant IC8x = 21389429792916570564174988170006717483165860817693539654312601487799928594075;
    uint256 constant IC8y = 19035465848321994517048194058485908737630784919803304169137076261671072924655;
    
    uint256 constant IC9x = 17099109859414476431562215036438467277803170844487487911514931814001063429292;
    uint256 constant IC9y = 7274321303078213212044491475233592795793384038879522164522888940114339907663;
    
 
    // Memory data
    uint16 constant pVk = 0;
    uint16 constant pPairing = 128;

    uint16 constant pLastMem = 896;

    function verifyProof(uint[2] calldata _pA, uint[2][2] calldata _pB, uint[2] calldata _pC, uint[9] calldata _pubSignals) public view returns (bool) {
        assembly {
            function checkField(v) {
                if iszero(lt(v, r)) {
                    mstore(0, 0)
                    return(0, 0x20)
                }
            }
            
            // G1 function to multiply a G1 value(x,y) to value in an address
            function g1_mulAccC(pR, x, y, s) {
                let success
                let mIn := mload(0x40)
                mstore(mIn, x)
                mstore(add(mIn, 32), y)
                mstore(add(mIn, 64), s)

                success := staticcall(sub(gas(), 2000), 7, mIn, 96, mIn, 64)

                if iszero(success) {
                    mstore(0, 0)
                    return(0, 0x20)
                }

                mstore(add(mIn, 64), mload(pR))
                mstore(add(mIn, 96), mload(add(pR, 32)))

                success := staticcall(sub(gas(), 2000), 6, mIn, 128, pR, 64)

                if iszero(success) {
                    mstore(0, 0)
                    return(0, 0x20)
                }
            }

            function checkPairing(pA, pB, pC, pubSignals, pMem) -> isOk {
                let _pPairing := add(pMem, pPairing)
                let _pVk := add(pMem, pVk)

                mstore(_pVk, IC0x)
                mstore(add(_pVk, 32), IC0y)

                // Compute the linear combination vk_x
                
                g1_mulAccC(_pVk, IC1x, IC1y, calldataload(add(pubSignals, 0)))
                
                g1_mulAccC(_pVk, IC2x, IC2y, calldataload(add(pubSignals, 32)))
                
                g1_mulAccC(_pVk, IC3x, IC3y, calldataload(add(pubSignals, 64)))
                
                g1_mulAccC(_pVk, IC4x, IC4y, calldataload(add(pubSignals, 96)))
                
                g1_mulAccC(_pVk, IC5x, IC5y, calldataload(add(pubSignals, 128)))
                
                g1_mulAccC(_pVk, IC6x, IC6y, calldataload(add(pubSignals, 160)))
                
                g1_mulAccC(_pVk, IC7x, IC7y, calldataload(add(pubSignals, 192)))
                
                g1_mulAccC(_pVk, IC8x, IC8y, calldataload(add(pubSignals, 224)))
                
                g1_mulAccC(_pVk, IC9x, IC9y, calldataload(add(pubSignals, 256)))
                

                // -A
                mstore(_pPairing, calldataload(pA))
                mstore(add(_pPairing, 32), mod(sub(q, calldataload(add(pA, 32))), q))

                // B
                mstore(add(_pPairing, 64), calldataload(pB))
                mstore(add(_pPairing, 96), calldataload(add(pB, 32)))
                mstore(add(_pPairing, 128), calldataload(add(pB, 64)))
                mstore(add(_pPairing, 160), calldataload(add(pB, 96)))

                // alpha1
                mstore(add(_pPairing, 192), alphax)
                mstore(add(_pPairing, 224), alphay)

                // beta2
                mstore(add(_pPairing, 256), betax1)
                mstore(add(_pPairing, 288), betax2)
                mstore(add(_pPairing, 320), betay1)
                mstore(add(_pPairing, 352), betay2)

                // vk_x
                mstore(add(_pPairing, 384), mload(add(pMem, pVk)))
                mstore(add(_pPairing, 416), mload(add(pMem, add(pVk, 32))))


                // gamma2
                mstore(add(_pPairing, 448), gammax1)
                mstore(add(_pPairing, 480), gammax2)
                mstore(add(_pPairing, 512), gammay1)
                mstore(add(_pPairing, 544), gammay2)

                // C
                mstore(add(_pPairing, 576), calldataload(pC))
                mstore(add(_pPairing, 608), calldataload(add(pC, 32)))

                // delta2
                mstore(add(_pPairing, 640), deltax1)
                mstore(add(_pPairing, 672), deltax2)
                mstore(add(_pPairing, 704), deltay1)
                mstore(add(_pPairing, 736), deltay2)


                let success := staticcall(sub(gas(), 2000), 8, _pPairing, 768, _pPairing, 0x20)

                isOk := and(success, mload(_pPairing))
            }

            let pMem := mload(0x40)
            mstore(0x40, add(pMem, pLastMem))

            // Validate that all evaluations ∈ F
            
            checkField(calldataload(add(_pubSignals, 0)))
            
            checkField(calldataload(add(_pubSignals, 32)))
            
            checkField(calldataload(add(_pubSignals, 64)))
            
            checkField(calldataload(add(_pubSignals, 96)))
            
            checkField(calldataload(add(_pubSignals, 128)))
            
            checkField(calldataload(add(_pubSignals, 160)))
            
            checkField(calldataload(add(_pubSignals, 192)))
            
            checkField(calldataload(add(_pubSignals, 224)))
            
            checkField(calldataload(add(_pubSignals, 256)))
            

            // Validate all evaluations
            let isValid := checkPairing(_pA, _pB, _pC, _pubSignals, pMem)

            mstore(0, isValid)
             return(0, 0x20)
         }
     }
 }
